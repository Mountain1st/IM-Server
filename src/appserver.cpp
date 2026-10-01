// ============================================================
// AppServer：业务层组装与总控（外层封装）
// 职责：
// - 构造/组装：网络层 TcpServer + 工作线程池 + 四仓库（用户/消息/
//   联系人/群）+ 可选 Redis 五件（token/限流/幂等/在线状态/查询限流）
// - 指令路由：buildRoutes 哈希表注册 29 条指令，onEnvelope 统一分发
//   （未登录仅放行 auth.register/login/token，其余 401）
// - 生命周期：start（线程启动）/ stop（四步优雅停机：
//   停接入 → Close(1001) 通知 → 工作队列排水 → 兜底关闭）
// - purgeLoop：后台清理线程（离线 TTL 7 天 + 历史保留期 90 天 +
//   联系人/群成员缓存计数比对自愈，默认 30 分钟一轮）
// 线程模型：本类自身方法多在工作线程池执行；session 回调注册在
// IO 线程；在线表 OnlineManager 内部有锁（跨线程安全）
// ============================================================
#include "appserver.hpp"

#include "connection.hpp"
#include "log.hpp"
#include "media_store.hpp"
#include "metrics.hpp"
#include "mysql_message_repository.hpp"
#include "mysql_user_repository.hpp"
#include "redis_msg_dedup.hpp"
#include "redis_presence_store.hpp"
#include "redis_rate_limiter.hpp"
#include "redis_token_store.hpp"
#include "server_config.hpp"
#include "app_util.hpp"
#include "session.hpp"

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace im {

namespace {
// 构造 MySQL 用户存储；配置为空（被环境变量置空）或连接失败直接抛错退出。
std::shared_ptr<UserRepository> makeUserRepository(const UserStoreConfig& c) {
  if (c.mysqlDb.empty() || c.mysqlUser.empty()) {
    throw std::runtime_error(
        "MySQL 配置为空：请勿用环境变量 IM_MYSQL_USER / IM_MYSQL_DB 置空默认配置");
  }
  auto repo = std::make_shared<MySqlUserRepository>(
      c.mysqlHost, c.mysqlPort, c.mysqlUser, c.mysqlPassword, c.mysqlDb);
  if (!repo->ok()) {
    throw std::runtime_error("MySQL 连接失败: " + repo->errStr());
  }
  LOG_I("[db] 用户存储: MySQL (host=" << c.mysqlHost << ":" << c.mysqlPort
        << ", db=" << c.mysqlDb << ")");
  return repo;
}
} // namespace

AppServer::AppServer(uint16_t port, size_t ioThreads, size_t workThreads)
    : AppServer(port, ioThreads, workThreads, 0) {}

AppServer::AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections)
    : AppServer(port, ioThreads, workThreads, maxConnections, {}) {}

AppServer::AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
                     std::vector<std::string> allowedOrigins)
    : AppServer(port, ioThreads, workThreads, maxConnections, std::move(allowedOrigins),
                UserStoreConfig{}) {}

AppServer::AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
                     std::vector<std::string> allowedOrigins, UserStoreConfig userConfig)
    : AppServer(port, ioThreads, workThreads, maxConnections, std::move(allowedOrigins),
                userConfig, RedisStoreConfig{}) {}

// 构造 Presence（在线状态）存储：Redis 启用则用 Redis（连接失败抛错），否则内存
std::shared_ptr<PresenceStore> makePresenceStore(const RedisStoreConfig& c) {
  if (!c.enabled) {
    LOG_I("[db] 在线状态: 内存");
    return std::make_shared<MemoryPresenceStore>();
  }
  auto rs = std::make_shared<RedisPresenceStore>(
      c.redisHost, c.redisPort, c.redisPassword, c.redisDb);
  if (!rs->ok()) {
    throw std::runtime_error("Redis 连接失败: " + rs->errStr());
  }
  LOG_I("[db] 在线状态: Redis (host=" << c.redisHost << ":" << c.redisPort
        << ", db=" << c.redisDb << ")");
  return rs;
}

// 构造 token 存储：Redis 启用则用 Redis（连接失败抛错），否则内存
std::shared_ptr<TokenStore> makeTokenStore(const RedisStoreConfig& c) {
  if (!c.enabled) {
    LOG_I("[db] token 存储: 内存");
    return std::make_shared<MemoryTokenStore>();
  }
  auto rs = std::make_shared<RedisTokenStore>(c);
  if (!rs->ok()) {
    throw std::runtime_error("Redis 连接失败: " + rs->errStr());
  }
  LOG_I("[db] token 存储: Redis (host=" << c.redisHost << ":" << c.redisPort
        << ", db=" << c.redisDb << ")");
  return rs;
}

// msg.send 限流：Redis 启用则用 ZSET 滑动窗口（跨重启/多实例），否则内存
std::unique_ptr<RateLimiterStore> makeRateLimiter(const RedisStoreConfig& c,
                                                const std::string& label,
                                                const std::string& keyPrefix = "rate:") {
  if (!c.enabled) {
    LOG_I("[db] " << label << ": 内存");
    return std::make_unique<MemoryRateLimiter>();
  }
  auto rs = std::make_unique<RedisRateLimiter>(c, keyPrefix);
  if (!rs->ok()) {
    throw std::runtime_error("Redis 连接失败: " + rs->errStr());
  }
  LOG_I("[db] " << label << ": Redis (host=" << c.redisHost << ":" << c.redisPort << ")");
  return rs;
}

// 消息幂等去重：Redis 启用则用 SET NX EX（跨重启/多实例），否则内存
std::unique_ptr<MsgDedupStore> makeMsgDedup(const RedisStoreConfig& c) {
  if (!c.enabled) {
    LOG_I("[db] 幂等去重: 内存");
    return std::make_unique<MemoryMsgDedup>();
  }
  auto rs = std::make_unique<RedisMsgDedup>(c);
  if (!rs->ok()) {
    throw std::runtime_error("Redis 连接失败: " + rs->errStr());
  }
  LOG_I("[db] 幂等去重: Redis (host=" << c.redisHost << ":" << c.redisPort << ")");
  return rs;
}

AppServer::AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
                     std::vector<std::string> allowedOrigins, UserStoreConfig userConfig,
                     RedisStoreConfig redisConfig, size_t maxPerIpConnections,
                     std::vector<std::string> trustedProxies)
    : tcpServer_(std::make_shared<TcpServer>(port, ioThreads, maxConnections,
                                             maxPerIpConnections)),
      workPool_(workThreads),
      allowedOrigins_(std::move(allowedOrigins)),
      trustedProxies_(std::move(trustedProxies)),
      userStore_(std::make_unique<UserStore>(makeUserRepository(userConfig),
                                             makeTokenStore(redisConfig))),
      // 初始化列表须与头文件成员声明顺序一致（-Wreorder）：
      // msgRepo/contactRepo/groupRepo → presence → onlineMgr(presence_) →
      // msgDedup/msgLimiter/queryLimiter
      msgRepo_(std::make_unique<MySqlMessageRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
      contactRepo_(std::make_unique<ContactRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
      groupRepo_(std::make_unique<GroupRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
      blockRepo_(std::make_unique<BlockRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
      presence_(makePresenceStore(redisConfig)),
      onlineMgr_(presence_),
      msgDedup_(makeMsgDedup(redisConfig)),
      msgLimiter_(makeRateLimiter(redisConfig, "发送限流")),
      queryLimiter_(makeRateLimiter(redisConfig, "查询限流", "qrate:")),
      registerLimiter_(makeRateLimiter(redisConfig, "注册限流", "reg:")),
      imgLimiter_(makeRateLimiter(redisConfig, "图片限流", "imgrate:")) {
  if (!msgRepo_->ok()) {
    throw std::runtime_error("消息存储连接失败");
  }
  LOG_I("[db] 消息存储: MySQL (host=" << userConfig.mysqlHost << ":" << userConfig.mysqlPort
        << ", db=" << userConfig.mysqlDb << ")");
  if (!contactRepo_->ok()) {
    throw std::runtime_error("联系人存储连接失败");
  }
  LOG_I("[db] 联系人存储: MySQL (host=" << userConfig.mysqlHost << ":"
        << userConfig.mysqlPort << ", db=" << userConfig.mysqlDb << ")");
  if (!groupRepo_->ok()) {
    throw std::runtime_error("群存储连接失败: " + groupRepo_->errStr());
  }
  if (!blockRepo_->ok()) {
    throw std::runtime_error("拉黑存储连接失败: " + blockRepo_->errStr());
  }
  LOG_I("[db] 拉黑存储: MySQL (host=" << userConfig.mysqlHost << ":"
        << userConfig.mysqlPort << ", db=" << userConfig.mysqlDb << ")");
  LOG_I("[db] 群存储: MySQL (host=" << userConfig.mysqlHost << ":"
        << userConfig.mysqlPort << ", db=" << userConfig.mysqlDb << ")");
  // 离线 TTL 清理间隔（秒）：环境变量可覆盖（测试用小值），非法值回退默认
  if (const char* v = std::getenv("IM_OFFLINE_PURGE_INTERVAL")) {
    const int n = std::atoi(v);
    if (n > 0) purgeIntervalSec_ = n;
  }
  // 注册限流额度（按 IP）：环境变量可覆盖（测试豁免用大值）
  if (const char* v = std::getenv("IM_REGISTER_LIMIT_PER_WINDOW")) {
    const int n = std::atoi(v);
    if (n > 0) registerLimit_ = static_cast<size_t>(n);
  }
  // 历史保留天数（已消费消息含群消息）：环境变量可覆盖；0 = 不清理
  if (const char* v = std::getenv("IM_MSG_RETENTION_DAYS")) {
    const int n = std::atoi(v);
    if (n >= 0) msgRetentionDays_ = n;
  }
  // 图片落盘目录：环境变量可覆盖（测试隔离用）；空值回退默认 ./uploads。
  // 启动时自建目录：建失败记错但不退出（图片发送时再判 500，不影响文字链路）
  if (const char* v = std::getenv("IM_UPLOADS_DIR")) {
    if (v[0] != '\0') uploadsDir_ = v;
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(uploadsDir_, ec);
    if (ec) {
      LOG_E("[media] 落盘目录自建失败：dir=" << uploadsDir_);
    }
  }
  // 媒体 GC 最短间隔：环境变量可覆盖（测试用小值验证 GC 调度）
  if (const char* v = std::getenv("IM_MEDIA_GC_INTERVAL_SEC")) {
    const int n = std::atoi(v);
    if (n > 0) mediaGcIntervalSec_ = n;
  }
  LOG_I("[purge] 消息清理线程已启动（间隔 " << purgeIntervalSec_
        << " 秒；离线 TTL "
        << std::chrono::duration_cast<std::chrono::hours>(config::kOfflineTtl).count()
        << " 天；历史保留 " << msgRetentionDays_ << " 天）");
  buildRoutes();
  purgeThread_ = std::thread([this] { purgeLoop(); });
}

void AppServer::buildRoutes() {
  routes_ = {
    {"auth.register", &AppServer::onRegister},
    {"auth.login", &AppServer::onLogin},
    {"auth.token", &AppServer::onToken},
    {"auth.logout", &AppServer::onLogout},
    {"msg.send", &AppServer::onSend},
    {"chat.create", &AppServer::onCreateConv},
    {"msg.offline_ack", &AppServer::onOfflineAck},
    {"chat.history", &AppServer::onHistory},
    {"msg.read", &AppServer::onRead},
    {"user.search", &AppServer::onSearch},
    {"contact.request", &AppServer::onContactRequest},
    {"contact.accept", &AppServer::onContactAccept},
    {"contact.reject", &AppServer::onContactReject},
    {"contact.list", &AppServer::onContactList},
    {"contact.requests", &AppServer::onContactRequests},
    {"contact.delete", &AppServer::onContactDelete},
    {"settings.get", &AppServer::onSettingsGet},
    {"settings.set", &AppServer::onSettingsSet},
    {"group.create", &AppServer::onGroupCreate},
    {"group.invite", &AppServer::onGroupInvite},
    {"group.leave", &AppServer::onGroupLeave},
    {"group.kick", &AppServer::onGroupKick},
    {"group.transfer", &AppServer::onGroupTransfer},
    {"group.dissolve", &AppServer::onGroupDissolve},
    {"group.rename", &AppServer::onGroupRename},
    {"group.announce", &AppServer::onGroupAnnounce},
    {"group.list", &AppServer::onGroupList},
    {"group.members", &AppServer::onGroupMembers},
    {"group.history", &AppServer::onGroupHistory},
    {"block.add", &AppServer::onBlockAdd},
    {"block.remove", &AppServer::onBlockRemove},
    {"block.list", &AppServer::onBlockList},
  };
}

AppServer::~AppServer() {
  // 停清理线程（wait_for 被唤醒后立即退出，不会阻塞析构）
  purgeStop_.store(true);
  {
    std::lock_guard<std::mutex> lk(purgeMutex_);
    purgeCv_.notify_all();
  }
  if (purgeThread_.joinable()) purgeThread_.join();
}

void AppServer::purgeLoop() {
  while (!purgeStop_.load()) {
    std::unique_lock<std::mutex> lk(purgeMutex_);
    purgeCv_.wait_for(lk, std::chrono::seconds(purgeIntervalSec_),
                      [this] { return purgeStop_.load(); });
    if (purgeStop_.load()) break;
    lk.unlock();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    // 兜底 catch（C3 自检三轮）：清理线程异常逃逸到线程入口会
    // std::terminate 全进程（如极端 OOM 的 bad_alloc）
    try {
      const int64_t removed = msgRepo_->purgeExpiredOffline(now);
      if (removed > 0) {
        LOG_I("[purge] 已清理 " << removed << " 条过期离线消息");
      }
      const int64_t aged = msgRepo_->purgeAgedHistory(now, msgRetentionDays_);
      if (aged > 0) {
        LOG_I("[purge] 已归档清理 " << aged << " 条超期历史消息");
      }
      // 同轮做内存缓存低频比对（默认 30 分钟一轮；四条 COUNT 成本可忽略）
      syncCaches();
      // 媒体 GC：距上次执行超过间隔才跑一轮（默认 24 小时；mtime 超 7 天的
      // 落盘文件删除，过期即 404 占位语义）。lastMediaGc_ 初值 0 使启动后
      // 首个 purge 轮即清一次陈旧文件（符合预期）；目录不存在时直接返回 0。
      if (now - lastMediaGc_ >= mediaGcIntervalSec_) {
        lastMediaGc_ = now;
        MediaStore::gcExpired(uploadsDir_, config::kMediaRetentionSec);
      }
    } catch (const std::exception& e) {
      LOG_E("[purge] 清理轮异常被兜底捕获: " << e.what());
    } catch (...) {
      LOG_E("[purge] 清理轮异常（未知类型）被兜底捕获");
    }
  }
}

void AppServer::syncCaches() {
  // 联系人缓存：库行数与缓存条目数不一致（绕过服务端直改库）→ 全量重建。
  // 计数查询失败跳过本轮（fail-safe：不动缓存）
  size_t dbN = 0;
  if (contactRepo_->dbContactCount(&dbN)) {
    const size_t cachedN = contactRepo_->cacheContactCount();
    if (dbN != cachedN) {
      LOG_I("[cache] 联系人缓存计数漂移（库 " << dbN << " / 缓存 " << cachedN
            << "），触发重建");
      contactRepo_->reloadContactsCache();
    }
  }
  // 群成员缓存：同构比对（现成员行数 vs 缓存条目数）
  size_t dbM = 0;
  if (groupRepo_->dbMemberCount(&dbM)) {
    const size_t cachedM = groupRepo_->cacheMemberCount();
    if (dbM != cachedM) {
      LOG_I("[cache] 群成员缓存计数漂移（库 " << dbM << " / 缓存 " << cachedM
            << "），触发重建");
      groupRepo_->reloadMembersCache();
    }
  }
  // 拉黑缓存（v2.4 第四段：与其他三缓存同构；拉黑是安全语义，
  // 直改库漂移不及时自愈会让本该拦截的消息放行）
  size_t dbB = 0;
  if (blockRepo_->dbBlockCount(&dbB)) {
    const size_t cachedB = blockRepo_->cacheBlockCount();
    if (dbB != cachedB) {
      LOG_I("[cache] 拉黑缓存计数漂移（库 " << dbB << " / 缓存 " << cachedB
            << "），触发重建");
      blockRepo_->reloadBlocksCache();
    }
  }
  // 用户 id 缓存（2026-09-30 自检三轮 B3 补齐——此前用户缓存是唯一
  // 无自愈路径的：加载失败即 userExists/fromNick miss 到重启）
  size_t dbU = 0;
  if (userStore_->dbUserCacheCount(&dbU)) {
    const size_t cachedU = userStore_->cacheUserCount();
    if (dbU != cachedU) {
      LOG_I("[cache] 用户缓存计数漂移（库 " << dbU << " / 缓存 " << cachedU
            << "），触发重建");
      userStore_->reloadUserCache();
    }
  }
}

void AppServer::start() {
  // 1) 通知工作线程启动
  workPool_.start();
  // 2) 通知 TcpServer：它内部再通知主从 eventloop 启动（主 loop 由 run() 运行）
  tcpServer_->setNewConnectionCallback(
    [this](EventLoop* ioLoop, const TcpServer::ConnectionPtr& conn) {
      onNewConnection(ioLoop, conn);
    });
  tcpServer_->start();
}

void AppServer::stop() {
  // 优雅停机四步（顺序不可换）：
  // 1) 停止接入：主 loop 退出（不再 accept 新连接；若信号路径已停则幂等）
  tcpServer_->requestStop();
  // 2) 通知在线会话：发 WS Close(1001 Going Away) 并关闭连接。
  //    此刻从 IO loop 仍运行（帧可写出）；连接关闭后不再有新业务入队
  tcpServer_->gracefulShutdown();
  // 3) 排水：工作线程把队列中已入队任务执行完（消息已落库的部分不受
  //    影响；回复写往已关连接会被静默丢弃，客户端靠离线补发自愈）。
  //    带超时防个别慢任务（DB 超时等）拖住停机
  workPool_.stop();
  // 4) 兜底：关闭残余连接（1001 写完即关的多数已自关）并停止从 loop
  tcpServer_->stop();
}

void AppServer::run() {
  tcpServer_->run();
}

void AppServer::requestStop() {
  tcpServer_->requestStop();
}

void AppServer::onNewConnection(EventLoop* ioLoop, const TcpServer::ConnectionPtr& conn) {
  auto session = std::make_shared<Session>(ioLoop, &workPool_, conn, allowedOrigins_,
                                           trustedProxies_);
  auto self = shared_from_this();
  // 会话关闭：先从在线表摘除，再通知网络层移除连接。
  // 注意：closedHandler 是 Session 成员，必须捕获弱引用，避免 Session 自引用循环泄漏。
  std::weak_ptr<Session> weakSession = session;
  session->setClosedHandler([self, weakSession] {
    if (auto s = weakSession.lock()) self->onSessionClosed(s);
  });
  // 业务入口：一条完整文本消息（在工作线程池线程执行）
  session->setTextHandler(
    [self](const std::shared_ptr<Session>& s, const std::string& text) {
      json env;
      if (!parseEnvelope(text, &env)) {
        s->reply(makeError(400, "无效的封包"));
        return;
      }
      self->onEnvelope(s, env);
    });
  // 在连接所属的从 loop 线程上启动
  ioLoop->runInLoop([session] { session->start(); });
  LOG_I("[app] 新会话 fd=" << conn->fd() << " 已创建");
}

void AppServer::onSessionClosed(const std::shared_ptr<Session>& session) {
  if (session->isLoggedIn()) {
    onlineMgr_.remove(session->userId(), session);
    LOG_I("[app] 用户 " << session->userId() << " 会话关闭");
  }
  tcpServer_->removeConnection(session->conn());
}

void AppServer::onEnvelope(const std::shared_ptr<Session>& session, const json& env) {
  try {
    const std::string type = env[env::kType].get<std::string>();
    const json& data = env[env::kData];

    // 未登录只放行 auth.register / auth.login / auth.token 三条；
    // 其余指令（含未知指令）一律 401，与路由表化之前的拦截顺序一致
    const bool authCmd = type == "auth.register" || type == "auth.login" ||
                         type == "auth.token";
    if (!authCmd && !session->isLoggedIn()) {
      session->reply(makeError(401, "未登录"));
      return;
    }
    auto it = routes_.find(type);
    if (it == routes_.end()) {
      session->reply(makeError(400, "未知的指令类型"));
      return;
    }
    (this->*it->second)(session, data);
  } catch (const json::exception& e) {
    // data.value() 在字段存在但类型不符时抛 type_error.302。
    // 统一按坏包处理并回复，避免异常逃逸到工作线程导致进程崩溃。
    LOG_E("[app] 封包字段类型异常: " << e.what());
    session->reply(makeError(400, "封包字段错误"));
  }
}

} // namespace im
