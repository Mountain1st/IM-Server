#include "appserver.hpp"
#include "app_util.hpp"
#include "log.hpp"
#include "connection.hpp"
#include "metrics.hpp"
#include "session.hpp"

namespace im {

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
  // 新用户注册即拥有"自己"联系人（联系人列表恒含自己）
  contactRepo_->addSelfContact(userId);
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
    LOG_E("[app] 登录时连接已关闭，userId=" << userId);
    return;
  }
  onlineMgr_.add(userId, session);
  if (session->conn()->isClosed()) {
    onlineMgr_.remove(userId, session);
    LOG_E("[app] 登录后连接随即关闭，撤销在线登记 userId=" << userId);
    return;
  }
  session->reply(makeResult("auth.login_res", 0, "ok",
      json{{"userId", userId}, {"account", account}, {"username", username}, {"token", token}}));
  // 登录应答之后补发离线消息（非破坏性：补发中断不丢失，确认前重登可再补发）
  pushOfflineMessages(userId, session);
  // 补发待处理的好友请求（登录后客户端才能收到 contact.request 推送）
  pushPendingRequests(userId, session);
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
    LOG_E("[app] token 重连时连接已关闭，userId=" << userId);
    return;
  }
  onlineMgr_.add(userId, session);
  if (session->conn()->isClosed()) {
    onlineMgr_.remove(userId, session);
    LOG_E("[app] token 重连后连接随即关闭，撤销在线登记 userId=" << userId);
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

} // namespace im
