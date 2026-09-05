#include "appserver.hpp"

#include "connection.hpp"
#include "metrics.hpp"
#include "mysql_message_repository.hpp"
#include "mysql_user_repository.hpp"
#include "redis_msg_dedup.hpp"
#include "redis_rate_limiter.hpp"
#include "redis_token_store.hpp"
#include "server_config.hpp"
#include "session.hpp"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <stdexcept>

namespace im {

namespace {
// 业务参数限制统一取自集中配置（server_config.hpp）
constexpr size_t kMaxContentBytes = config::kMaxContentBytes;
constexpr size_t kMinAccountLen = config::kMinAccountLen;
constexpr size_t kMaxAccountLen = config::kMaxAccountLen;
constexpr size_t kLoginAccountMaxLen = config::kLoginAccountMaxLen;
constexpr size_t kMaxUsernameLen = config::kMaxUsernameLen;
constexpr size_t kMinPasswordLen = config::kMinPasswordLen;
constexpr size_t kMaxPasswordLen = config::kMaxPasswordLen;
// msg.send 限流：窗口时长内每用户最多发送条数；空 clientMsgId 无法参与去重，走更严限制
constexpr std::chrono::seconds kSendWindow(config::kSendWindow);
constexpr size_t kMaxSendPerWindow = config::kMaxSendPerWindow;
constexpr size_t kMaxSendNoIdPerWindow = config::kMaxSendNoIdPerWindow;

// 账号格式：10-11 位纯数字（如手机号）。
// 注册与新建对话（chat.create）均按此校验，不合规回 400；
// 登录不校验格式（错误统一按 403 处理，兼容遗留账号，不泄露账号存在性）。
bool validAccount(const std::string& a) {
  if (a.size() < kMinAccountLen || a.size() > kMaxAccountLen) return false;
  for (char c : a) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// 密码规则校验：6-32 位、仅字母/数字、且同时包含字母与数字
bool validPassword(const std::string& p) {
  if (p.size() < kMinPasswordLen || p.size() > kMaxPasswordLen) return false;
  bool hasLetter = false, hasDigit = false;
  for (unsigned char c : p) {
    if (c >= '0' && c <= '9') {
      hasDigit = true;
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      hasLetter = true;
    } else {
      return false; // 含非字母数字字符
    }
  }
  return hasLetter && hasDigit; // 必须字母与数字都有
}

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
  std::cout << "[db] 用户存储: MySQL (host=" << c.mysqlHost << ":" << c.mysqlPort
            << ", db=" << c.mysqlDb << ")" << std::endl;
  return repo;
}

// 解析消息收发目标字段（msg.send 的 to / chat.history 的 to / msg.read 的 from）。
// 协议 v1.8：字段为登录账号 account；
// 兼容旧协议传数字 userId。自己对话框通过 to==自己 account 表达，
// 不再有 to:0 特例。
// 返回值：>0=目标 userId；-1=参数错误（400）；-2=目标不存在（404）；-3=存储异常（500）。
int64_t resolveTarget(UserStore* store, const json& data, const char* key) {
  if (!data.contains(key)) return -1;
  const json& v = data[key];
  std::string s;
  if (v.is_string()) {
    s = v.get<std::string>();
  } else if (v.is_number_integer()) {
    s = std::to_string(v.get<int64_t>());
  } else {
    return -1;
  }
  if (s.empty()) return -1;
  const bool digits = std::all_of(s.begin(), s.end(),
                                  [](char c) { return c >= '0' && c <= '9'; });
  // 非 10-11 位数字：视为旧协议 userId 数字（如 121），按 userId 校验
  if (!digits || s.size() < kMinAccountLen || s.size() > kMaxAccountLen) {
    int64_t uid = 0;
    try {
      uid = std::stoll(s);
    } catch (...) {
      return -1;
    }
    if (uid <= 0) return -1;
    if (!store->exists(uid)) return -2;
    return uid;
  }
  // 10-11 位数字：按登录账号 account 解析
  int64_t uid = 0;
  std::string nick;
  const int frc = store->lookupAccount(s, &uid, &nick);
  if (frc == 2) return -3;
  if (frc != 0) return -2;
  return uid;
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

// 构造 token 存储：Redis 启用则用 Redis（连接失败抛错），否则内存
std::shared_ptr<TokenStore> makeTokenStore(const RedisStoreConfig& c) {
  if (!c.enabled) {
    std::cout << "[db] token 存储: 内存" << std::endl;
    return std::make_shared<MemoryTokenStore>();
  }
  auto rs = std::make_shared<RedisTokenStore>(c);
  if (!rs->ok()) {
    throw std::runtime_error("Redis 连接失败: " + rs->errStr());
  }
  std::cout << "[db] token 存储: Redis (host=" << c.redisHost << ":" << c.redisPort
            << ", db=" << c.redisDb << ")" << std::endl;
  return rs;
}

// msg.send 限流：Redis 启用则用 ZSET 滑动窗口（跨重启/多实例），否则内存
std::unique_ptr<RateLimiterStore> makeRateLimiter(const RedisStoreConfig& c) {
  if (!c.enabled) {
    std::cout << "[db] 发送限流: 内存" << std::endl;
    return std::make_unique<MemoryRateLimiter>();
  }
  auto rs = std::make_unique<RedisRateLimiter>(c);
  if (!rs->ok()) {
    throw std::runtime_error("Redis 连接失败: " + rs->errStr());
  }
  std::cout << "[db] 发送限流: Redis (host=" << c.redisHost << ":" << c.redisPort << ")"
            << std::endl;
  return rs;
}

// 消息幂等去重：Redis 启用则用 SET NX EX（跨重启/多实例），否则内存
std::unique_ptr<MsgDedupStore> makeMsgDedup(const RedisStoreConfig& c) {
  if (!c.enabled) {
    std::cout << "[db] 幂等去重: 内存" << std::endl;
    return std::make_unique<MemoryMsgDedup>();
  }
  auto rs = std::make_unique<RedisMsgDedup>(c);
  if (!rs->ok()) {
    throw std::runtime_error("Redis 连接失败: " + rs->errStr());
  }
  std::cout << "[db] 幂等去重: Redis (host=" << c.redisHost << ":" << c.redisPort << ")"
            << std::endl;
  return rs;
}

AppServer::AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
                     std::vector<std::string> allowedOrigins, UserStoreConfig userConfig,
                     RedisStoreConfig redisConfig)
    : tcpServer_(std::make_shared<TcpServer>(port, ioThreads, maxConnections)),
      workPool_(workThreads),
      allowedOrigins_(std::move(allowedOrigins)),
      userStore_(std::make_unique<UserStore>(makeUserRepository(userConfig),
                                             makeTokenStore(redisConfig))),
      msgDedup_(makeMsgDedup(redisConfig)),
      msgLimiter_(makeRateLimiter(redisConfig)),
      msgRepo_(std::make_unique<MySqlMessageRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
      contactRepo_(std::make_unique<ContactRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)) {
  if (!msgRepo_->ok()) {
    throw std::runtime_error("消息存储连接失败");
  }
  std::cout << "[db] 消息存储: MySQL (host=" << userConfig.mysqlHost << ":" << userConfig.mysqlPort
            << ", db=" << userConfig.mysqlDb << ")" << std::endl;
  if (!contactRepo_->ok()) {
    throw std::runtime_error("联系人存储连接失败");
  }
  std::cout << "[db] 联系人存储: MySQL (host=" << userConfig.mysqlHost << ":"
            << userConfig.mysqlPort << ", db=" << userConfig.mysqlDb << ")" << std::endl;
}

AppServer::~AppServer() = default;

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
  auto session = std::make_shared<Session>(ioLoop, &workPool_, conn, allowedOrigins_);
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
  std::cout << "[app] 新会话 fd=" << conn->fd() << " 已创建" << std::endl;
}

void AppServer::onSessionClosed(const std::shared_ptr<Session>& session) {
  if (session->isLoggedIn()) {
    onlineMgr_.remove(session->userId(), session);
    std::cout << "[app] 用户 " << session->userId() << " 会话关闭" << std::endl;
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
    session->reply(makeError(400, "未知的指令类型"));
  } catch (const json::exception& e) {
    // data.value() 在字段存在但类型不符时抛 type_error.302。
    // 统一按坏包处理并回复，避免异常逃逸到工作线程导致进程崩溃。
    std::cerr << "[app] 封包字段类型异常: " << e.what() << std::endl;
    session->reply(makeError(400, "封包字段错误"));
  }
}

void AppServer::onRegister(const std::shared_ptr<Session>& session, const json& data) {
  std::string account = data.value("account", "");
  std::string password = data.value("password", "");
  // username 为昵称（可选，空则缺省与账号一致）
  std::string username = data.value("username", "");
  if (username.empty()) username = account;
  // 校验：账号须为 10-11 位数字；密码 6-32 位字母数字组合；昵称 < 32 位。
  // 账号格式不合规给专用消息（客户端可直接展示原因）
  if (!validAccount(account)) {
    session->reply(makeError(400, "账号须为10-11位数字"));
    return;
  }
  if (!validPassword(password) || username.size() >= kMaxUsernameLen) {
    session->reply(makeError(400, "参数错误"));
    return;
  }
  int64_t userId = 0;
  int rc = userStore_->registerUser(account, password, username, &userId);
  if (rc != 0) {
    session->reply(makeResult("auth.register_res", rc, "账号已存在", json::object()));
    return;
  }
  session->reply(makeResult("auth.register_res", 0, "ok",
      json{{"userId", userId}, {"account", account}, {"username", username}}));
}

void AppServer::onLogin(const std::shared_ptr<Session>& session, const json& data) {
  std::string account = data.value("account", "");
  std::string password = data.value("password", "");
  // 登录：输入仅做宽松上限校验（防滥用），格式不校验——
  // 账号不存在与密码错误统一回 403，不泄露账号存在性
  if (account.empty() || account.size() > kLoginAccountMaxLen ||
      password.empty() || password.size() > kMaxPasswordLen) {
    session->reply(makeError(400, "参数错误"));
    return;
  }
  int64_t userId = 0;
  std::string username;
  std::string token;
  int rc = userStore_->login(account, password, &userId, &username, &token);
  if (rc != 0) {
    g_metrics.authFailures.fetch_add(1, std::memory_order_relaxed);
    // 透传真实错误码：403 密码错误 / 429 登录限流 / 500 存储异常
    const char* msg = (rc == 429) ? "登录尝试过于频繁，请稍后再试"
                    : (rc == 500) ? "服务器内部错误"
                    : "账号或密码错误";
    session->reply(makeResult("auth.login_res", rc, msg, json::object()));
    return;
  }
  // 防重复登录：本会话若已登录其它账号，先注销旧绑定（在线表+token），
  // 否则在线表会残留指向本会话的旧 userId 条目，导致发给旧用户的消息被错投。
  if (session->isLoggedIn()) {
    onlineMgr_.remove(session->userId(), session);
    userStore_->logout(session->token());
  }
  session->setLogin(userId, token);
  // 连接可能恰在此时被 loop 线程关闭，先检查避免往在线表加已死会话。
  // 注意 isClosed() 检查与 add 之间存在窗口，add 后需二次复查：
  // 若期间连接被关闭，立即撤销在线表登记，避免残留已死会话条目。
  if (session->conn()->isClosed()) {
    std::cerr << "[app] 登录时连接已关闭，userId=" << userId << std::endl;
    return;
  }
  onlineMgr_.add(userId, session);
  if (session->conn()->isClosed()) {
    onlineMgr_.remove(userId, session);
    std::cerr << "[app] 登录后连接随即关闭，撤销在线登记 userId=" << userId << std::endl;
    return;
  }
  session->reply(makeResult("auth.login_res", 0, "ok",
      json{{"userId", userId}, {"account", account}, {"username", username}, {"token", token}}));
  // 登录应答之后补发离线消息（非破坏性：补发中断不丢失，确认前重登可再补发）
  pushOfflineMessages(userId, session);
  // 补发待处理的好友请求（登录后客户端才能收到 contact.request 推送）
  pushPendingRequests(userId, session);
}

// 生成全局消息 ID：时间戳前缀 + 进程内自增序列（重启后前缀变化，避免跨进程碰撞）
std::string AppServer::nextMessageId() {
  uint64_t seq = msgSeq_.fetch_add(1, std::memory_order_relaxed) + 1;
  return std::to_string(static_cast<int64_t>(std::time(nullptr))) + "-" +
         std::to_string(seq);
}

void AppServer::pushOfflineMessages(int64_t userId,
                                    const std::shared_ptr<Session>& session) {
  // 从消息存储读取该用户的离线消息（status=offline_pending），按 ts 升序
  auto stored = msgRepo_->fetchOffline(userId);
  if (stored.empty()) return;
  json arr = json::array();
  for (const auto& m : stored) {
    std::string fromNick;
    userStore_->nickname(m.fromUser, &fromNick);
    std::string fromAccount;
    if (!userStore_->accountOf(m.fromUser, &fromAccount)) fromAccount.clear();
    arr.push_back(json{
      {"from", fromAccount},
      {"fromNick", fromNick},
      {"content", m.content},
      {"ts", m.ts},
      {"clientMsgId", m.clientMsgId},
      {"messageId", m.messageId},
      {"chatType", m.chatType},
    });
  }
  session->reply(makeEnvelopeText("msg.offline", json{{"msgs", arr}}));
}

void AppServer::onToken(const std::shared_ptr<Session>& session, const json& data) {
  std::string token = data.value("token", "");
  if (token.empty()) {
    session->reply(makeError(400, "参数错误"));
    return;
  }
  int64_t userId = 0;
  std::string username;
  int rc = userStore_->verifyToken(token, &userId, &username);
  if (rc != 0) {
    session->reply(makeResult("auth.token_res", 401, "登录凭证无效或已过期", json::object()));
    return;
  }
  // 防重复登录：本会话若已登录其它账号，先注销旧绑定（在线表+token），
  // 否则在线表会残留指向本会话的旧 userId 条目。
  if (session->isLoggedIn()) {
    onlineMgr_.remove(session->userId(), session);
    userStore_->logout(session->token());
  }
  session->setLogin(userId, token);
  if (session->conn()->isClosed()) {
    std::cerr << "[app] token 重连时连接已关闭，userId=" << userId << std::endl;
    return;
  }
  onlineMgr_.add(userId, session);
  if (session->conn()->isClosed()) {
    onlineMgr_.remove(userId, session);
    std::cerr << "[app] token 重连后连接随即关闭，撤销在线登记 userId=" << userId << std::endl;
    return;
  }
  session->reply(makeResult("auth.token_res", 0, "ok",
      json{{"userId", userId}, {"username", username}, {"token", token}}));
  // 重连成功后同样补发离线消息（非破坏性）
  pushOfflineMessages(userId, session);
  // 重连补发待处理的好友请求
  pushPendingRequests(userId, session);
}

void AppServer::onLogout(const std::shared_ptr<Session>& session) {
  int64_t userId = session->userId();
  onlineMgr_.remove(userId, session);
  userStore_->logout(session->token());
  session->logout();
  session->reply(makeResult("auth.logout_res", 0, "ok", json::object()));
}

void AppServer::onSend(const std::shared_ptr<Session>& session, const json& data) {
  std::string content = data.value("content", "");
  std::string clientMsgId = data.value("clientMsgId", "");
  auto ack = [&](int code, const std::string& msg) {
    session->reply(makeEnvelopeText("msg.ack",
        json{{"code", code}, {"msg", msg}, {"clientMsgId", clientMsgId}}));
  };
  // chatType 仅支持单聊；缺省按 "single" 处理（协议 §3）
  std::string chatType = data.value("chatType", "single");
  if (chatType != "single") {
    ack(400, "封包格式错误");
    return;
  }
  if (content.empty()) {
    ack(400, "参数错误");
    return;
  }
  if (content.size() > kMaxContentBytes) {
    ack(413, "消息内容超长（超过64KB）");
    return;
  }
  // to 为目标登录账号 account（协议 v1.7；兼容旧 userId 数字与 0=自己对话框）
  const int64_t to = resolveTarget(userStore_.get(), data, "to");
  if (to == -1) {
    ack(400, "参数错误");
    return;
  }
  if (to == -2) {
    ack(404, "目标用户不存在");
    return;
  }
  if (to == -3) {
    ack(500, "服务器内部错误");
    return;
  }
  // 权限校验：自己对话框 / 联系人 / 目标开启临时对话
  if (to != session->userId()) {
    int allowTemp = 0;
    const bool isContact = contactRepo_->isContact(session->userId(), to);
    if (!isContact) {
      const int frc = userStore_->fetchSettings(to, &allowTemp, nullptr, nullptr);
      if (frc != 0) { // 目标不存在或 DB 错误（resolveTarget 已查过，此处兜底）
        ack(500, "服务器内部错误");
        return;
      }
      if (allowTemp != 1) {
        ack(403, "对方未开启临时对话，请先添加联系人");
        return;
      }
    }
  }
  // 幂等查询（只查不记）：窗口内已处理过 → 直接回执成功、不再投递。
  // 放在限流之前，使合法重试不消耗发送配额。
  if (msgDedup_->isDuplicate(session->userId(), clientMsgId)) {
    ack(0, "ok");
    return;
  }
  // 消息发送限流：空 clientMsgId 不参与去重，走更严限制，遏制无 id 刷消息。
  // 限流失败不记录幂等，避免被拒的请求在重试时被误判为已发送。
  const size_t sendLimit = clientMsgId.empty() ? kMaxSendNoIdPerWindow : kMaxSendPerWindow;
  if (!msgLimiter_->allow(session->userId(), sendLimit, kSendWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    ack(429, "消息发送过于频繁，请稍后再试");
    return;
  }
  // 自己对话框（to==自己账号）：落库持久化，只回 ack 不回显 msg.recv（发送方即自己）
  if (to == session->userId()) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::string messageId = nextMessageId();
    if (!msgRepo_->saveMessage(messageId, session->userId(), to, content, now,
                               clientMsgId, chatType, kMsgDelivered)) {
      ack(500, "服务器内部错误");
      return;
    }
    msgDedup_->remember(session->userId(), clientMsgId);
    ack(0, "ok");
    return;
  }
  // 目标在线与否决定消息初始状态：在线=delivered，离线=offline_pending
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string messageId = nextMessageId();
  const bool targetOnline = !onlineMgr_.sessions(to).empty();
  const int status = targetOnline ? kMsgDelivered : kMsgOfflinePending;
  // 先持久化成功再确认（避免"内存入队伪装成持久化"）
  if (!msgRepo_->saveMessage(messageId, session->userId(), to, content, now,
                             clientMsgId, chatType, status)) {
    ack(500, "服务器内部错误");
    return;
  }
  msgDedup_->remember(session->userId(), clientMsgId);
  ack(0, "ok");
  // 发送方昵称与账号（供接收方/离线消息直接展示，无需客户端另查；
  // from 为登录账号 account，userId 对客户端不透明）
  std::string fromNick;
  if (!userStore_->nickname(session->userId(), &fromNick)) fromNick.clear();
  std::string fromAccount;
  if (!userStore_->accountOf(session->userId(), &fromAccount)) fromAccount.clear();
  json recvData = {
    {"from", fromAccount},
    {"fromNick", fromNick},
    {"content", content},
    {"ts", now},
    {"clientMsgId", clientMsgId},
    {"messageId", messageId},
    {"chatType", chatType},
  };
  if (targetOnline) {
    for (auto& t : onlineMgr_.sessions(to)) {
      t->reply(makeEnvelopeText("msg.recv", recvData));
    }
    g_metrics.messagesDelivered.fetch_add(1, std::memory_order_relaxed);
  } else {
    // 目标离线：已持久化为 offline_pending，待其登录后补发
    g_metrics.messagesOffline.fetch_add(1, std::memory_order_relaxed);
  }
}

void AppServer::onOfflineAck(const std::shared_ptr<Session>& session, const json& data) {
  // 客户端确认已收到 msg.offline 中的消息，服务端标记为已投递
  std::vector<std::string> messageIds;
  if (data.contains("messageIds") && data["messageIds"].is_array()) {
    for (const auto& v : data["messageIds"]) {
      if (v.is_string()) messageIds.push_back(v.get<std::string>());
    }
  }
  msgRepo_->markDelivered(session->userId(), messageIds);
  session->reply(makeResult("msg.offline_ack_res", 0, "ok", json::object()));
}

void AppServer::onHistory(const std::shared_ptr<Session>& session, const json& data) {
  // 拉取与目标用户的单聊历史（按 ts 倒序，最新在前）。
  // to 为目标登录账号 account（协议 v1.7；兼容旧 userId 数字与 0=自己对话框）。
  const int64_t to = resolveTarget(userStore_.get(), data, "to");
  if (to == -1) {
    session->reply(makeResult("chat.history_res", 400, "参数错误", json::object()));
    return;
  }
  if (to == -2) {
    session->reply(makeResult("chat.history_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (to == -3) {
    session->reply(makeResult("chat.history_res", 500, "服务器内部错误", json::object()));
    return;
  }
  int limit = data.value("limit", 50);
  auto stored = msgRepo_->fetchHistory(session->userId(), to, limit);
  json arr = json::array();
  for (const auto& m : stored) {
    std::string fromNick;
    userStore_->nickname(m.fromUser, &fromNick);
    std::string fromAccount;
    if (!userStore_->accountOf(m.fromUser, &fromAccount)) fromAccount.clear();
    arr.push_back(json{
      {"from", fromAccount},
      {"fromNick", fromNick},
      {"content", m.content},
      {"ts", m.ts},
      {"clientMsgId", m.clientMsgId},
      {"messageId", m.messageId},
      {"chatType", m.chatType},
    });
  }
  session->reply(makeResult("chat.history_res", 0, "ok", json{{"msgs", arr}}));
}

void AppServer::onRead(const std::shared_ptr<Session>& session, const json& data) {
  // 已读回执：把与某用户、截至 ts 的未读消息标记为已读。
  // from 为目标登录账号 account（协议 v1.7；兼容旧 userId 数字与 0=自己）。
  const int64_t from = resolveTarget(userStore_.get(), data, "from");
  if (from == -1 || from == -2 || from == -3) {
    session->reply(makeResult("msg.read_res", 400, "参数错误", json::object()));
    return;
  }
  // 自己对话框：无对端消息，直接成功
  if (from == 0 || from == session->userId()) {
    session->reply(makeResult("msg.read_res", 0, "ok", json::object()));
    return;
  }
  int64_t ts = data.value("ts", static_cast<int64_t>(std::time(nullptr)));
  msgRepo_->markRead(session->userId(), from, ts);
  session->reply(makeResult("msg.read_res", 0, "ok", json::object()));
}

void AppServer::onSearch(const std::shared_ptr<Session>& session, const json& data) {
  std::string keyword = data.value("keyword", "");
  if (keyword.empty()) {
    session->reply(makeResult("user.search_res", 400, "参数错误", json::object()));
    return;
  }
  int limit = data.value("limit", 20);
  std::vector<std::pair<std::string, std::string>> users;
  userStore_->searchUsers(keyword, session->userId(), limit, &users);
  json arr = json::array();
  for (const auto& u : users) {
    int64_t uid = 0;
    std::string nick;
    userStore_->lookupAccount(u.first, &uid, &nick);
    arr.push_back(json{{"account", u.first}, {"username", u.second},
                       {"isContact", contactRepo_->isContact(session->userId(), uid)}});
  }
  session->reply(makeResult("user.search_res", 0, "ok", json{{"users", arr}}));
}

void AppServer::onContactRequest(const std::shared_ptr<Session>& session, const json& data) {
  const std::string account = data.value("account", "");
  const int64_t to = resolveTarget(userStore_.get(), data, "account");
  if (to == -1) {
    session->reply(makeResult("contact.request_res", 400, "参数错误", json::object()));
    return;
  }
  if (to == -2) {
    session->reply(makeResult("contact.request_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (to == -3) {
    session->reply(makeResult("contact.request_res", 500, "服务器内部错误", json::object()));
    return;
  }
  const int64_t self = session->userId();
  if (to == self) {
    session->reply(makeResult("contact.request_res", 400, "不能添加自己为联系人", json::object()));
    return;
  }
  if (contactRepo_->isContact(self, to)) {
    session->reply(makeResult("contact.request_res", 409, "对方已是你的联系人", json::object()));
    return;
  }
  int allowAdd = 0;
  const int frc = userStore_->fetchSettings(to, nullptr, nullptr, &allowAdd);
  if (frc != 0) {
    session->reply(makeResult("contact.request_res", 500, "服务器内部错误", json::object()));
    return;
  }
  if (allowAdd != 1) {
    session->reply(makeResult("contact.request_res", 403, "对方未开启添加，请使用其他方式联系", json::object()));
    return;
  }
  const int rc = contactRepo_->addRequest(self, to);
  if (rc == 1) {
    session->reply(makeResult("contact.request_res", 409, "已存在待处理的请求", json::object()));
    return;
  }
  if (rc == 2) {
    session->reply(makeResult("contact.request_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("contact.request_res", 0, "ok", json::object()));
  // 推送给被发起人（在线）；离线由登录后补发（pushPendingRequests）
  std::string fromNick, fromAccount;
  userStore_->nickname(self, &fromNick);
  userStore_->accountOf(self, &fromAccount);
  json ev = {{"from", fromAccount}, {"fromNick", fromNick},
             {"ts", static_cast<int64_t>(std::time(nullptr))}};
  for (auto& t : onlineMgr_.sessions(to)) {
    t->reply(makeEnvelopeText("contact.request", ev));
  }
}

void AppServer::onContactAccept(const std::shared_ptr<Session>& session, const json& data) {
  const int64_t from = resolveTarget(userStore_.get(), data, "account");
  if (from == -1 || from == -2 || from == -3) {
    session->reply(makeResult("contact.accept_res", 400, "参数错误", json::object()));
    return;
  }
  const int rc = contactRepo_->acceptRequest(session->userId(), from);
  if (rc == 1) {
    session->reply(makeResult("contact.accept_res", 404, "不存在该待处理请求", json::object()));
    return;
  }
  if (rc == 2) {
    session->reply(makeResult("contact.accept_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("contact.accept_res", 0, "ok", json::object()));
  // 通知请求方：成为联系人
  std::string account, username;
  userStore_->accountOf(session->userId(), &account);
  userStore_->nickname(session->userId(), &username);
  json ev = {{"account", account}, {"username", username}};
  for (auto& t : onlineMgr_.sessions(from)) {
    t->reply(makeEnvelopeText("contact.added", ev));
  }
}

void AppServer::onContactReject(const std::shared_ptr<Session>& session, const json& data) {
  const int64_t from = resolveTarget(userStore_.get(), data, "account");
  if (from == -1 || from == -2 || from == -3) {
    session->reply(makeResult("contact.reject_res", 400, "参数错误", json::object()));
    return;
  }
  const int rc = contactRepo_->rejectRequest(session->userId(), from);
  if (rc == 1) {
    session->reply(makeResult("contact.reject_res", 404, "不存在该待处理请求", json::object()));
    return;
  }
  if (rc == 2) {
    session->reply(makeResult("contact.reject_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("contact.reject_res", 0, "ok", json::object()));
}

void AppServer::onContactList(const std::shared_ptr<Session>& session, const json& data) {
  const auto ids = contactRepo_->listContactIds(session->userId());
  json arr = json::array();
  for (const int64_t id : ids) {
    std::string account, username;
    userStore_->accountOf(id, &account);
    userStore_->nickname(id, &username);
    arr.push_back(json{{"account", account}, {"username", username}});
  }
  session->reply(makeResult("contact.list_res", 0, "ok", json{{"contacts", arr}}));
}

void AppServer::onContactRequests(const std::shared_ptr<Session>& session, const json& data) {
  const auto reqs = contactRepo_->listPendingRequests(session->userId());
  json arr = json::array();
  for (const auto& r : reqs) {
    std::string account, username;
    userStore_->accountOf(r.fromUser, &account);
    userStore_->nickname(r.fromUser, &username);
    arr.push_back(json{{"account", account}, {"username", username}, {"ts", r.ts}});
  }
  session->reply(makeResult("contact.requests_res", 0, "ok", json{{"requests", arr}}));
}

void AppServer::onContactDelete(const std::shared_ptr<Session>& session, const json& data) {
  const int64_t other = resolveTarget(userStore_.get(), data, "account");
  if (other == -1 || other == -2 || other == -3) {
    session->reply(makeResult("contact.delete_res", 400, "参数错误", json::object()));
    return;
  }
  if (other == session->userId()) {
    session->reply(makeResult("contact.delete_res", 400, "参数错误", json::object()));
    return;
  }
  if (!contactRepo_->isContact(session->userId(), other)) {
    session->reply(makeResult("contact.delete_res", 404, "对方不是你的联系人", json::object()));
    return;
  }
  contactRepo_->deleteContactPair(session->userId(), other);
  session->reply(makeResult("contact.delete_res", 0, "ok", json::object()));
  // 通知对方（在线）
  std::string account;
  userStore_->accountOf(session->userId(), &account);
  json ev = {{"account", account}};
  for (auto& t : onlineMgr_.sessions(other)) {
    t->reply(makeEnvelopeText("contact.deleted", ev));
  }
}

void AppServer::onSettingsGet(const std::shared_ptr<Session>& session, const json& data) {
  int allowTemp = 0, searchable = 0, allowAdd = 0;
  const int rc = userStore_->fetchSettings(session->userId(), &allowTemp, &searchable, &allowAdd);
  if (rc != 0) {
    session->reply(makeResult("settings.get_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("settings.get_res", 0, "ok",
      json{{"allowTempChat", allowTemp == 1},
           {"searchable", searchable == 1},
           {"allowAdd", allowAdd == 1}}));
}

void AppServer::onSettingsSet(const std::shared_ptr<Session>& session, const json& data) {
  const int allowTemp = data.contains("allowTempChat") ? (data["allowTempChat"].get<bool>() ? 1 : 0) : -1;
  const int searchable = data.contains("searchable") ? (data["searchable"].get<bool>() ? 1 : 0) : -1;
  const int allowAdd = data.contains("allowAdd") ? (data["allowAdd"].get<bool>() ? 1 : 0) : -1;
  userStore_->updateSettings(session->userId(), allowTemp, searchable, allowAdd);
  session->reply(makeResult("settings.set_res", 0, "ok", json::object()));
}

void AppServer::pushPendingRequests(int64_t userId,
                                    const std::shared_ptr<Session>& session) {
  const auto reqs = contactRepo_->listPendingRequests(userId);
  if (reqs.empty()) return;
  for (const auto& r : reqs) {
    std::string account, username;
    userStore_->accountOf(r.fromUser, &account);
    userStore_->nickname(r.fromUser, &username);
    json ev = {{"from", account}, {"fromNick", username}, {"ts", r.ts}};
    session->reply(makeEnvelopeText("contact.request", ev));
  }
}

void AppServer::onCreateConv(const std::shared_ptr<Session>& session, const json& data) {
  // 按登录账号 account 查找目标用户；存在则返回其 account 与昵称。
  // 账号须为 10-11 位数字：格式不合规直接 400（如输入 "0" 不再误报 404）
  std::string account = data.value("account", "");
  if (!validAccount(account)) {
    session->reply(makeResult("chat.create_res", 400, "账号须为10-11位数字", json::object()));
    return;
  }
  int64_t uid = 0;
  std::string username;
  int frc = userStore_->lookupAccount(account, &uid, &username);
  if (frc == 2) {
    session->reply(makeResult("chat.create_res", 500, "服务器内部错误", json::object()));
    return;
  }
  if (frc != 0) {
    // 账号不存在：明确告知客户端
    session->reply(makeResult("chat.create_res", 404, "目标用户不存在", json::object()));
    return;
  }
  // 自己对话框：请求的目标账号即当前登录账号时（uid == 自己），
  // 与旧 to:0 逻辑一致——返回自己的信息（此处即 uid/account/username 均为自己）。
  session->reply(makeResult("chat.create_res", 0, "ok",
      json{{"userId", uid}, {"account", account}, {"username", username}}));
}

} // namespace im
