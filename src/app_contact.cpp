#include "appserver.hpp"
#include "app_util.hpp"
#include "log.hpp"
#include "session.hpp"

#include <ctime>

namespace im {

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
  int64_t reqTs = static_cast<int64_t>(std::time(nullptr));
  contactRepo_->requestTime(self, to, &reqTs);  // 失败则回退本地时间
  json ev = {{"from", fromAccount}, {"fromNick", fromNick}, {"ts", reqTs}};
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
  const int64_t self = session->userId();
  // "自己"联系人恒排第一位（contact_id == 自己 userId）
  for (const int64_t id : ids) {
    if (id == self) continue;
    std::string account, username;
    userStore_->accountOf(id, &account);
    userStore_->nickname(id, &username);
    arr.push_back(json{{"account", account}, {"username", username},
                       {"online", onlineMgr_.isOnlinePresence(id)}});
  }
  std::string selfAccount, selfNick;
  userStore_->accountOf(self, &selfAccount);
  userStore_->nickname(self, &selfNick);
  json selfEntry = json{{"account", selfAccount}, {"username", selfNick},
                        {"online", onlineMgr_.isOnlinePresence(self)}};
  json out = json::array();
  out.push_back(selfEntry);
  for (const auto& e : arr) out.push_back(e);
  session->reply(makeResult("contact.list_res", 0, "ok", json{{"contacts", out}}));
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

} // namespace im
