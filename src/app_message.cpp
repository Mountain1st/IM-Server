#include "appserver.hpp"
#include "app_util.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "session.hpp"

#include <algorithm>
#include <ctime>

namespace im {

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
  // 群离线补发：各群按水位线查行（ts > watermark 且在成员在群期间内），与单聊行按 ts 合并
  int64_t maxTs = 0;
  for (const auto& m : stored) maxTs = std::max(maxTs, m.ts);
  for (const int64_t gid : groupRepo_->listUserGroups(userId)) {
    int64_t joined = 0, left = 0;
    if (!groupRepo_->memberPeriod(gid, userId, &joined, &left)) continue;
    const int64_t wm = groupRepo_->getWatermark(gid, userId);
    auto grows = msgRepo_->fetchGroupMessages(gid, std::max(wm, joined - 1), left, 100, true);
    std::string gname;
    groupRepo_->getGroup(gid, &gname, nullptr);
    for (const auto& m : grows) {
      std::string fromNick, fromAccount;
      userStore_->nickname(m.fromUser, &fromNick);
      if (!userStore_->accountOf(m.fromUser, &fromAccount)) fromAccount.clear();
      arr.push_back(json{{"from", fromAccount},
                         {"fromNick", fromNick},
                         {"groupId", gid},
                         {"groupName", gname},
                         {"content", m.content},
                         {"ts", m.ts},
                         {"clientMsgId", m.clientMsgId},
                         {"messageId", m.messageId},
                         {"chatType", "group"}});
      maxTs = std::max(maxTs, m.ts);
      groupRepo_->setWatermark(gid, userId, m.ts);
    }
  }
  if (!arr.empty()) {
    // 按 ts 升序合并（单聊行已升序，群行逐群升序追加后整体排序）
    std::sort(arr.begin(), arr.end(),
              [](const json& a, const json& b) { return a["ts"] < b["ts"]; });
  }
  if (arr.empty()) return;
  session->reply(makeEnvelopeText("msg.offline", json{{"msgs", arr}}));
}

void AppServer::onSend(const std::shared_ptr<Session>& session, const json& data) {
  std::string content = data.value("content", "");
  std::string clientMsgId = data.value("clientMsgId", "");
  auto ack = [&](int code, const std::string& msg) {
    session->reply(makeEnvelopeText("msg.ack",
        json{{"code", code}, {"msg", msg}, {"clientMsgId", clientMsgId}}));
  };
  // chatType：缺省 "single"；"group" 走群分支（协议 v1.9）；其他值拒绝
  std::string chatType = data.value("chatType", "single");
  if (chatType == "group") {
    onGroupSend(session, data, content, clientMsgId);
    return;
  }
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
  // 幂等查询（只查不记）：窗口内已处理过 → 直接回执成功、不再投递。
  // 放在限流之前，使合法重试不消耗发送配额；且不受权限状态影响（重发恒回 ack 0）。
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
  // 权限校验：自己对话框 / 联系人 / 目标开启临时对话。
  // 位于限流之后、自己对话框落库之前：被限流拒绝不触发权限校验；
  // 自己对话框（to==自己）跳过权限，直接落库。
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
