#include "appserver.hpp"

#include "connection.hpp"
#include "metrics.hpp"
#include "mysql_user_repository.hpp"
#include "server_config.hpp"
#include "session.hpp"

#include <ctime>
#include <iostream>
#include <stdexcept>

namespace im {

namespace {
// 业务参数限制统一取自集中配置（server_config.hpp）
constexpr size_t kMaxContentBytes = config::kMaxContentBytes;
constexpr size_t kMaxAccountLen = config::kMaxAccountLen;
constexpr size_t kMaxUsernameLen = config::kMaxUsernameLen;
constexpr size_t kMinPasswordLen = config::kMinPasswordLen;
constexpr size_t kMaxPasswordLen = config::kMaxPasswordLen;
// msg.send 限流：窗口时长内每用户最多发送条数；空 clientMsgId 无法参与去重，走更严限制
constexpr std::chrono::seconds kSendWindow(config::kSendWindow);
constexpr size_t kMaxSendPerWindow = config::kMaxSendPerWindow;
constexpr size_t kMaxSendNoIdPerWindow = config::kMaxSendNoIdPerWindow;

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
    : tcpServer_(std::make_shared<TcpServer>(port, ioThreads, maxConnections)),
      workPool_(workThreads),
      allowedOrigins_(std::move(allowedOrigins)),
      userStore_(std::make_unique<UserStore>(makeUserRepository(userConfig))) {}

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
  // 校验：账号非空且不超长；密码 6-32 位字母数字组合；昵称 < 32 位
  if (account.empty() || account.size() > kMaxAccountLen ||
      !validPassword(password) ||
      username.size() >= kMaxUsernameLen) {
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
  if (account.empty() || account.size() > kMaxAccountLen ||
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
  // 登录应答之后补发离线消息（非破坏性 peek：补发中断不丢失，重登可再补发）
  auto msgs = offlineStore_.peek(userId);
  if (!msgs.empty()) {
    session->reply(makeEnvelopeText("msg.offline", json{{"msgs", msgs}}));
  }
}

// 生成全局消息 ID：时间戳前缀 + 进程内自增序列（重启后前缀变化，避免跨进程碰撞）
std::string AppServer::nextMessageId() {
  uint64_t seq = msgSeq_.fetch_add(1, std::memory_order_relaxed) + 1;
  return std::to_string(static_cast<int64_t>(std::time(nullptr))) + "-" +
         std::to_string(seq);
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
  auto msgs = offlineStore_.peek(userId);
  if (!msgs.empty()) {
    session->reply(makeEnvelopeText("msg.offline", json{{"msgs", msgs}}));
  }
}

void AppServer::onLogout(const std::shared_ptr<Session>& session) {
  int64_t userId = session->userId();
  onlineMgr_.remove(userId, session);
  userStore_->logout(session->token());
  session->logout();
  session->reply(makeResult("auth.logout_res", 0, "ok", json::object()));
}

void AppServer::onSend(const std::shared_ptr<Session>& session, const json& data) {
  int64_t to = data.value("to", (int64_t)0);
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
  if (to < 0 || content.empty()) {
    ack(400, "参数错误");
    return;
  }
  if (content.size() > kMaxContentBytes) {
    ack(413, "消息内容超长（超过64KB）");
    return;
  }
  // to 为目标用户 userId；to == 0 为「自己对话框」（协议 §4），不校验目标存在性
  if (to != 0 && !userStore_->exists(to)) {
    ack(404, "目标用户不存在");
    return;
  }
  // 幂等查询（只查不记）：窗口内已处理过 → 直接回执成功、不再投递。
  // 放在限流之前，使合法重试不消耗发送配额。
  if (msgDedup_.isDuplicate(session->userId(), clientMsgId)) {
    ack(0, "ok");
    return;
  }
  // 消息发送限流：空 clientMsgId 不参与去重，走更严限制，遏制无 id 刷消息。
  // 限流失败不记录幂等，避免被拒的请求在重试时被误判为已发送。
  const size_t sendLimit = clientMsgId.empty() ? kMaxSendNoIdPerWindow : kMaxSendPerWindow;
  if (!msgLimiter_.allow(session->userId(), sendLimit, kSendWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    ack(429, "消息发送过于频繁，请稍后再试");
    return;
  }
  ack(0, "ok");
  // 消息已确认接受（在线投递或离线入队），此时才记录幂等，供窗口内重试去重
  msgDedup_.remember(session->userId(), clientMsgId);
  // 自己对话框：to == 0 或目标为自己的 userId → 只回 ack 不回发 msg.recv，
  // 消息由客户端本地显示（协议 §4 约束）。
  if (to == 0 || to == session->userId()) return;
  // 发送方昵称（供接收方/离线消息直接展示，无需客户端另查）
  std::string fromNick;
  if (!userStore_->nickname(session->userId(), &fromNick)) fromNick.clear();
  json recvData = {
    {"from", session->userId()},
    {"fromNick", fromNick},
    {"content", content},
    {"ts", static_cast<int64_t>(std::time(nullptr))},
    {"clientMsgId", clientMsgId},
    {"messageId", nextMessageId()},
    {"chatType", chatType},
  };
  auto targets = onlineMgr_.sessions(to);
  if (targets.empty()) {
    // 目标离线：暂存，待其登录后补发
    offlineStore_.push(to, recvData);
    g_metrics.messagesOffline.fetch_add(1, std::memory_order_relaxed);
  } else {
    for (auto& t : targets) {
      t->reply(makeEnvelopeText("msg.recv", recvData));
    }
    g_metrics.messagesDelivered.fetch_add(targets.size(), std::memory_order_relaxed);
  }
}

void AppServer::onCreateConv(const std::shared_ptr<Session>& session, const json& data) {
  // 按登录账号 account 查找目标用户；存在则返回其 account 与昵称
  std::string account = data.value("account", "");
  if (account.empty() || account.size() > kMaxAccountLen) {
    session->reply(makeResult("chat.create_res", 400, "参数错误", json::object()));
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
