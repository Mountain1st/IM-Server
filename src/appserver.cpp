#include "appserver.hpp"

#include "connection.hpp"
#include "log.hpp"
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
std::unique_ptr<RateLimiterStore> makeRateLimiter(const RedisStoreConfig& c) {
  if (!c.enabled) {
    LOG_I("[db] 发送限流: 内存");
    return std::make_unique<MemoryRateLimiter>();
  }
  auto rs = std::make_unique<RedisRateLimiter>(c);
  if (!rs->ok()) {
    throw std::runtime_error("Redis 连接失败: " + rs->errStr());
  }
  LOG_I("[db] 发送限流: Redis (host=" << c.redisHost << ":" << c.redisPort << ")");
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
      msgDedup_(makeMsgDedup(redisConfig)),
      msgLimiter_(makeRateLimiter(redisConfig)),
      msgRepo_(std::make_unique<MySqlMessageRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
      contactRepo_(std::make_unique<ContactRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
      groupRepo_(std::make_unique<GroupRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
      presence_(makePresenceStore(redisConfig)),
      onlineMgr_(presence_) {
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
  LOG_I("[db] 群存储: MySQL (host=" << userConfig.mysqlHost << ":"
        << userConfig.mysqlPort << ", db=" << userConfig.mysqlDb << ")");
  // 离线 TTL 清理间隔（秒）：环境变量可覆盖（测试用小值），非法值回退默认
  if (const char* v = std::getenv("IM_OFFLINE_PURGE_INTERVAL")) {
    const int n = std::atoi(v);
    if (n > 0) purgeIntervalSec_ = n;
  }
  LOG_I("[purge] 离线消息 TTL 清理线程已启动（间隔 " << purgeIntervalSec_
        << " 秒，保留 "
        << std::chrono::duration_cast<std::chrono::hours>(config::kOfflineTtl).count()
        << " 天）");
  purgeThread_ = std::thread([this] { purgeLoop(); });
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
    const int64_t removed = msgRepo_->purgeExpiredOffline(
        static_cast<int64_t>(std::time(nullptr)));
    if (removed > 0) {
      LOG_I("[purge] 已清理 " << removed << " 条过期离线消息");
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
  // 1) 停止工作线程
  workPool_.stop();
  // 2) 通知 TcpServer 停止，由其通知主从 eventloop
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

    if (type == "auth.register") {
      onRegister(session, data);
      return;
    }
    if (type == "auth.login") {
      onLogin(session, data);
      return;
    }
    if (type == "auth.token") {
      onToken(session, data);
      return;
    }
    // 未登录只允许 auth.register / auth.login / auth.token
    if (!session->isLoggedIn()) {
      session->reply(makeError(401, "未登录"));
      return;
    }
    if (type == "auth.logout") {
      onLogout(session);
      return;
    }
    if (type == "msg.send") {
      onSend(session, data);
      return;
    }
    if (type == "chat.create") {
      onCreateConv(session, data);
      return;
    }
    if (type == "msg.offline_ack") {
      onOfflineAck(session, data);
      return;
    }
    if (type == "chat.history") {
      onHistory(session, data);
      return;
    }
    if (type == "msg.read") {
      onRead(session, data);
      return;
    }
    if (type == "user.search") {
      onSearch(session, data);
      return;
    }
    if (type == "contact.request") {
      onContactRequest(session, data);
      return;
    }
    if (type == "contact.accept") {
      onContactAccept(session, data);
      return;
    }
    if (type == "contact.reject") {
      onContactReject(session, data);
      return;
    }
    if (type == "contact.list") {
      onContactList(session, data);
      return;
    }
    if (type == "contact.requests") {
      onContactRequests(session, data);
      return;
    }
    if (type == "contact.delete") {
      onContactDelete(session, data);
      return;
    }
    if (type == "settings.get") {
      onSettingsGet(session, data);
      return;
    }
    if (type == "settings.set") {
      onSettingsSet(session, data);
      return;
    }
    if (type == "group.create") {
      onGroupCreate(session, data);
      return;
    }
    if (type == "group.invite") {
      onGroupInvite(session, data);
      return;
    }
    if (type == "group.leave") {
      onGroupLeave(session, data);
      return;
    }
    if (type == "group.kick") { onGroupKick(session, data); return; }
    if (type == "group.transfer") { onGroupTransfer(session, data); return; }
    if (type == "group.dissolve") { onGroupDissolve(session, data); return; }
    if (type == "group.rename") { onGroupRename(session, data); return; }
    if (type == "group.announce") { onGroupAnnounce(session, data); return; }
    if (type == "group.list") {
      onGroupList(session, data);
      return;
    }
    if (type == "group.members") {
      onGroupMembers(session, data);
      return;
    }
    if (type == "group.history") {
      onGroupHistory(session, data);
      return;
    }
    session->reply(makeError(400, "未知的指令类型"));
  } catch (const json::exception& e) {
    // data.value() 在字段存在但类型不符时抛 type_error.302。
    // 统一按坏包处理并回复，避免异常逃逸到工作线程导致进程崩溃。
    LOG_E("[app] 封包字段类型异常: " << e.what());
    session->reply(makeError(400, "封包字段错误"));
  }
}

} // namespace im
