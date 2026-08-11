#include "appserver.hpp"

#include "connection.hpp"
#include "session.hpp"

#include <ctime>
#include <iostream>

namespace im {

namespace {
// 单条消息 content 长度上限（与 docs/protocol.md 一致）
constexpr size_t kMaxContentBytes = 64 * 1024;
}

AppServer::AppServer(uint16_t port, size_t ioThreads, size_t workThreads)
    : AppServer(port, ioThreads, workThreads, 0) {}

AppServer::AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections)
    : tcpServer_(std::make_shared<TcpServer>(port, ioThreads, maxConnections)),
      workPool_(workThreads) {}

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
  auto session = std::make_shared<Session>(ioLoop, &workPool_, conn);
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
        s->reply(makeError(400, "invalid packet"));
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
      session->reply(makeError(401, "NOT_LOGIN"));
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
    session->reply(makeError(400, "unknown type"));
  } catch (const json::exception& e) {
    // data.value() 在字段存在但类型不符时抛 type_error.302。
    // 统一按坏包处理并回复，避免异常逃逸到工作线程导致进程崩溃。
    std::cerr << "[app] 封包字段类型异常: " << e.what() << std::endl;
    session->reply(makeError(400, "bad packet"));
  }
}

void AppServer::onRegister(const std::shared_ptr<Session>& session, const json& data) {
  std::string username = data.value("username", "");
  std::string password = data.value("password", "");
  if (username.empty() || password.empty()) {
    session->reply(makeError(400, "bad params"));
    return;
  }
  int64_t userId = 0;
  int rc = userStore_.registerUser(username, password, &userId);
  if (rc != 0) {
    session->reply(makeResult("auth.register_res", rc, "USER_EXISTS", {}));
    return;
  }
  session->reply(makeResult("auth.register_res", 0, "ok",
      json{{"userId", userId}, {"username", username}}));
}

void AppServer::onLogin(const std::shared_ptr<Session>& session, const json& data) {
  std::string username = data.value("username", "");
  std::string password = data.value("password", "");
  if (username.empty() || password.empty()) {
    session->reply(makeError(400, "bad params"));
    return;
  }
  int64_t userId = 0;
  std::string token;
  int rc = userStore_.login(username, password, &userId, &token);
  if (rc != 0) {
    // 透传真实错误码：403 密码错误 / 429 登录限流
    const std::string msg = (rc == 429) ? "TOO_MANY_ATTEMPTS" : "WRONG_PASSWORD";
    session->reply(makeResult("auth.login_res", rc, msg, {}));
    return;
  }
  // 防重复登录：本会话若已登录其它账号，先注销旧绑定（在线表+token），
  // 否则在线表会残留指向本会话的旧 userId 条目，导致发给旧用户的消息被错投。
  if (session->isLoggedIn()) {
    onlineMgr_.remove(session->userId(), session);
    userStore_.logout(session->token());
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
      json{{"userId", userId}, {"username", username}, {"token", token}}));
  // 登录应答之后补发离线消息
  auto msgs = offlineStore_.take(userId);
  if (!msgs.empty()) {
    session->reply(makeEnvelopeText("msg.offline", json{{"msgs", msgs}}));
  }
}

void AppServer::onToken(const std::shared_ptr<Session>& session, const json& data) {
  std::string token = data.value("token", "");
  if (token.empty()) {
    session->reply(makeError(400, "bad params"));
    return;
  }
  int64_t userId = 0;
  std::string username;
  int rc = userStore_.verifyToken(token, &userId, &username);
  if (rc != 0) {
    session->reply(makeResult("auth.token_res", 401, "INVALID_TOKEN", {}));
    return;
  }
  // 防重复登录：本会话若已登录其它账号，先注销旧绑定（在线表+token），
  // 否则在线表会残留指向本会话的旧 userId 条目。
  if (session->isLoggedIn()) {
    onlineMgr_.remove(session->userId(), session);
    userStore_.logout(session->token());
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
  // 重连成功后同样补发离线消息
  auto msgs = offlineStore_.take(userId);
  if (!msgs.empty()) {
    session->reply(makeEnvelopeText("msg.offline", json{{"msgs", msgs}}));
  }
}

void AppServer::onLogout(const std::shared_ptr<Session>& session) {
  int64_t userId = session->userId();
  onlineMgr_.remove(userId, session);
  userStore_.logout(session->token());
  session->logout();
  session->reply(makeResult("auth.logout_res", 0, "ok", {}));
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
    ack(400, "BAD_PACKET");
    return;
  }
  if (to <= 0 || content.empty()) {
    ack(400, "bad params");
    return;
  }
  if (content.size() > kMaxContentBytes) {
    ack(413, "MSG_TOO_LONG");
    return;
  }
  if (!userStore_.exists(to)) {
    ack(404, "USER_NOT_FOUND");
    return;
  }
  // 幂等去重：同一发送方在窗口内重发相同 clientMsgId，直接回执成功但不再投递
  if (msgDedup_.checkAndRemember(session->userId(), clientMsgId)) {
    ack(0, "ok");
    return;
  }
  json recvData = {
    {"from", session->userId()},
    {"content", content},
    {"ts", static_cast<int64_t>(std::time(nullptr))},
    {"clientMsgId", clientMsgId},
  };
  auto targets = onlineMgr_.sessions(to);
  if (targets.empty()) {
    // 目标离线：暂存，待其登录后补发
    offlineStore_.push(to, recvData);
  } else {
    for (auto& t : targets) {
      t->reply(makeEnvelopeText("msg.recv", recvData));
    }
  }
  ack(0, "ok");
}

} // namespace im
