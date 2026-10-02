// ============================================================
// 业务域：消息收发核心（msg.* / chat.*）——服务端最热路径
// - onSend：单聊/群聊统一入口。处理顺序（有依赖，勿调整）：
//   msgType 白名单（text/image，兼容 msg_type，非字符串 400）→ 幂等查询
//   （窗口内同 clientMsgId 直接回 ack 0；空 id 跳过；不消耗限流额度，
//   图片重发不重复限流/校验）→ image 走媒体分支
//   （mediaId=fileId 缺失/非串 400 → 经 media 表校验归属 403/存在 404/
//   COMMITTED 400 → 取 media.id 落库，content 存 fileId；目标/发送限流/
//   权限预检 400/404/403/429；ack 不带 msg_type；URL 由客户端调 media.url）
//   → 参数校验（to 解析；image 已预检通过，直接复用）→ 限流（30 条/10s；
//   空 clientMsgId 更严 5 条；image 已预检消费，不再重复）
//   → 权限校验（image 已预检通过，直接复用）
//   （自己对话框跳过 / 联系人 / allow_temp_chat 临时对话，否则 403）
//   → 落库（持久化，重启不丢）→ 投递（在线）或记 pending（离线）
//   → 多端回显（发送者其他在线端，v2.2）
// - onGroupSend：群发（onSend 按 chatType 分流而来）：成员校验 →
//   单行落库（to_user=0, group_id=G）→ 逐成员投递/按水位线记 pending
// - onOfflineAck：客户端确认离线消息已收到（标记 status，防重复补发；
//   单包 ≤1000 条防海量 UPDATE）
// - onHistory / onGroupHistory：单聊/群历史拉取（受查询限流 20 次/10s）
// - onRead：已读回执（更新 read_ts，并推送 msg.read_notify 给其他端，
//   有会话往来门槛防 userId 枚举）
// - onRecall：消息撤回（发送者+120s 窗内；存储层单条 UPDATE 原子校验；
//   recall_notify 扇出对端/群成员/多端；重复撤回幂等）
// - onConversationList：会话列表（v3.2 派生式；单聊未读出自 status；
//   群 unread/lastReadTs 恒 0——投递水位线≠已读游标，群已读 v3.x 独立启用）
// - pushOfflineMessages：登录/重连后离线消息补发（非破坏性）
// - nextMessageId：全局消息 ID（时间戳前缀 + 进程内自增，防重启碰撞）
// 线程：工作线程池（AppServer::onEnvelope 分发）
// ============================================================
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

json AppServer::messageToJson(const StoredMessage& m, const std::string& groupName) {
  // 撤回终态：正文/媒体在唯一出口剥离（四出口共用本函数，防单点遗漏）
  const bool recalled = (m.status == kMsgRecalled);
  std::string fromNick;
  userStore_->nickname(m.fromUser, &fromNick);
  std::string fromAccount;
  if (!userStore_->accountOf(m.fromUser, &fromAccount)) fromAccount.clear();
  json j = {
    {"from", fromAccount},
    {"fromNick", fromNick},
    {"content", recalled ? std::string() : m.content},
    {"ts", m.ts},
    {"clientMsgId", m.clientMsgId},
    {"messageId", m.messageId},
    {"chatType", m.chatType},
    {"msgType", m.msgType},
    // 媒体消息透传 fileId（存于 content）；撤回后剥离
    {"mediaId", (!recalled && m.msgType == "image") ? m.content : std::string()},
  };
  if (recalled) j["recalled"] = true;
  if (m.groupId != 0) {
    j["groupId"] = m.groupId;
    if (!groupName.empty()) j["groupName"] = groupName;
  }
  return j;
}

void AppServer::pushOfflineMessages(int64_t userId,
                                    const std::shared_ptr<Session>& session) {
  // 从消息存储读取该用户的离线消息（status=offline_pending），按 ts 升序
  auto stored = msgRepo_->fetchOffline(userId);
  json arr = json::array();
  for (const auto& m : stored) {
    arr.push_back(messageToJson(m));
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
      if (m.fromUser == userId) continue;      // 跳过发送者自己的消息（与在线路径一致）
      if (m.status == kMsgRecalled) continue;  // 已撤回不补发（未见过的人不收占位符）
      arr.push_back(messageToJson(m, gname));
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

int AppServer::precheckImageTarget(const std::shared_ptr<Session>& session, const json& data,
                                   const std::string& chatType, int64_t* toOut,
                                   std::string* ackMsg) {
  // 图片解码前置校验（与原位校验同码同文案，行为一致）：
  // 先目标存在性，再发送限流，最后权限——任一失败直接回码，
  // 此时尚未落盘，不产生孤儿文件、不白耗图片配额；失败不记幂等。
  const std::string cid = data.value("clientMsgId", "");
  const size_t sendLimit = cid.empty() ? kMaxSendNoIdPerWindow : kMaxSendPerWindow;
  auto allowSend = [&]() -> bool {
    if (!msgLimiter_->allow(session->userId(), sendLimit, kSendWindow)) {
      g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
      *ackMsg = "消息发送过于频繁，请稍后再试";
      return false;
    }
    return true;
  };
  if (chatType == "group") {
    if (!(data.contains("to") && data["to"].is_number_integer())) {
      *ackMsg = "参数错误";
      return 400;
    }
    const int64_t groupId = data["to"].get<int64_t>();
    std::string gname;
    int64_t ownerId = 0;
    bool dissolved = false;
    if (!groupRepo_->getGroup(groupId, &gname, &ownerId, nullptr, nullptr, &dissolved)) {
      *ackMsg = "群不存在";
      return 404;
    }
    if (dissolved) {
      *ackMsg = "群已解散";
      return 404;
    }
    if (!allowSend()) return 429;
    if (!groupRepo_->isMember(groupId, session->userId())) {
      *ackMsg = "非群成员";
      return 403;
    }
    *toOut = groupId;
    return 0;
  }
  const int64_t to = resolveTarget(userStore_.get(), data, "to");
  if (to == -1) {
    *ackMsg = "参数错误";
    return 400;
  }
  if (to == -2) {
    *ackMsg = "目标用户不存在";
    return 404;
  }
  if (to == -3) {
    *ackMsg = "服务器内部错误";
    return 500;
  }
  if (!allowSend()) return 429;
  if (to != session->userId()) {
    if (blockRepo_->isBlockedPair(session->userId(), to)) {
      if (blockRepo_->isOneWayBlockedBy(session->userId(), to)) {
        *ackMsg = "你已拉黑对方，请先解除";
      } else {
        *ackMsg = "对方已拒收";
      }
      return 403;
    }
    int allowTemp = 0;
    if (!contactRepo_->isContact(session->userId(), to)) {
      if (userStore_->fetchSettings(to, &allowTemp, nullptr, nullptr) != 0) {
        *ackMsg = "服务器内部错误";
        return 500;
      }
      if (allowTemp != 1) {
        *ackMsg = "对方未开启临时对话，请先添加联系人";
        return 403;
      }
    }
  }
  *toOut = to;
  return 0;
}

void AppServer::onSend(const std::shared_ptr<Session>& session, const json& data) {
  // 统一发送入口（单聊）。处理顺序（顺序有依赖，勿调整）：
  //   0 msgType 白名单（非字符串/白名单外 400）→ 1 幂等查询（窗口内同
  //     clientMsgId 直接回 ack 0——放在图片限流/解码与发送限流之前，
  //     合法重试不消耗配额、不重复落盘，且不受权限状态影响；空 id 跳过）
  //   2 图片分支（空/超 1.5MB 判 413 → base64 合法 400 → 图片限流 429 →
  //     并发闸解码落盘 400，content 换 URL 后并入文本流程）/ 文本内容校验
  //   3 to 解析（账号/兼容数字 userId/0=自己）→ 4 发送限流（30 条/10s；
  //     空 id 更严 5 条；被拒不记幂等防误判）→ 5 权限校验
  //     （to==自己跳过；联系人；allow_temp_chat）
  //   6 落库 → 7 在线投递/离线记 pending → 8 多端回显（v2.2）
  // chatType=group 时分流 onGroupSend（幂等/限流已完成）
  std::string content = data.value("content", "");
  std::string clientMsgId = data.value("clientMsgId", "");
  auto ack = [&](int code, const std::string& msg) {
    session->reply(makeEnvelopeText("msg.ack",
        json{{"code", code}, {"msg", msg}, {"clientMsgId", clientMsgId}}));
  };
  // clientMsgId 长度上限：与 DB 列宽对齐，超长直接拒绝（单聊/群聊同规则）
  if (clientMsgId.size() > kMaxClientMsgIdBytes) {
    ack(400, "参数错误");
    return;
  }
  // chatType：缺省 "single"；"group" 走群分支（协议 v1.9）；其他值拒绝。
  // msgType 解析 + 幂等查询 + 图片管线在分流之前（单聊/群聊共用）：
  // 合法重试不消耗图片配额、不重复落盘；图片解码后 content 换 URL 再分流。
  std::string chatType = data.value("chatType", "single");
  if (chatType != "single" && chatType != "group") {
    ack(400, "封包格式错误");
    return;
  }
  // 消息类型：缺省 "text"；兼容旧字段名 "msg_type"；非字符串类型
  // （如数字）与白名单外一律 400，不静默当 text。
  // ack 不带 msg_type（协议约定，客户端以发送上下文区分）。
  std::string msgType = "text";
  if (data.contains("msgType")) {
    if (!data["msgType"].is_string()) {
      ack(400, "参数错误");
      return;
    }
    msgType = data["msgType"].get<std::string>();
  } else if (data.contains("msg_type")) {
    if (!data["msg_type"].is_string()) {
      ack(400, "参数错误");
      return;
    }
    msgType = data["msg_type"].get<std::string>();
  }
  if (msgType != "text" && msgType != "image") {
    ack(400, "参数错误");
    return;
  }
  // 幂等查询（只查不记）：窗口内已处理过 → 直接回执成功、不再投递。
  // 放在 msgType 分流之后、图片媒体校验之前：合法重试不重复校验媒体；
  // 同样在文本限流之前，合法重试不消耗发送配额；
  // 且不受权限状态影响（重发恒回 ack 0）。空 clientMsgId 跳过（不参与去重）。
  if (!clientMsgId.empty() && msgDedup_->isDuplicate(session->userId(), clientMsgId)) {
    ack(0, "ok");
    return;
  }
  int64_t precheckTo = 0;  // 图片预检通过的目标（单聊=userId，群聊=groupId）
  int64_t imageMediaId = 0;  // 图片消息关联的 media.id（DB media_id）
  std::string imageFileId;   // 图片消息 fileId（下发透传用；文本为空）
  if (msgType == "image") {
    // 媒体消息（V1 仅图片）：不再走 WS 内联 base64——客户端先 presign 直传
    // 对象存储并 commit，msg.send 只携带 mediaId（fileId 字符串）。
    // 校验该媒体属当前用户且已 COMMITTED，取 media.id 落库；content 存 fileId
    // 供下发/历史透传（URL 由客户端按需调 media.url 获取，服务端不下发）。
    if (!data.contains("mediaId") || !data["mediaId"].is_string()) {
      ack(400, "参数错误");
      return;
    }
    const std::string fileId = data["mediaId"].get<std::string>();
    if (fileId.empty()) {
      ack(400, "参数错误");
      return;
    }
    if (!mediaRepo_ || !mediaRepo_->ok()) {
      ack(500, "服务器内部错误");
      return;
    }
    auto rec = mediaRepo_->findByFileId(fileId);
    if (!rec) {
      ack(404, "媒体不存在");
      return;
    }
    if (rec->uploaderId != session->userId()) {
      ack(403, "无权操作该媒体");
      return;
    }
    if (rec->status != kMediaCommitted) {
      ack(400, "媒体未提交");
      return;
    }
    imageMediaId = rec->id;
    imageFileId = fileId;
    content = fileId;
    // 目标/发送限流/权限预检（原图片限流路径复用）：无效目标与无权限请求
    // 在此拦截，失败不记幂等、不落库。
    std::string preMsg;
    const int preCode =
        precheckImageTarget(session, data, chatType, &precheckTo, &preMsg);
    if (preCode != 0) {
      ack(preCode, preMsg);
      return;
    }
  }
  if (chatType == "group") {
    onGroupSend(session, data, content, clientMsgId, msgType, imageMediaId);
    return;
  }
  if (content.empty()) {
    ack(400, "参数错误");
    return;
  } else if (content.size() > kMaxContentBytes) {
    ack(413, "消息内容超长（超过64KB）");
    return;
  }
  // to 为目标登录账号 account（协议 v1.7；兼容旧 userId 数字与 0=自己对话框）。
  // 图片路径上游预检已做目标解析 + 发送限流 + 权限校验，直接复用结果
  // （不再重复消费发送额度）；文本路径沿用原位校验（行为不变）。
  int64_t to = -1;
  if (msgType == "image") {
    to = precheckTo;
  } else {
    to = resolveTarget(userStore_.get(), data, "to");
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
  }
  if (msgType == "text") {
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
    // 拉黑检查（v2.4 双向断联）：优先级最高——即使开着临时对话/是
    // 联系人也不可达（此时联系人已被拉黑联动删除，双保险）。
    // 方向性报错：拉黑方提示先解除；被拉黑方提示对方拒收。
    // 拒绝不落库、不记幂等（与限流拒绝同先例，重试不被误判）
    if (blockRepo_->isBlockedPair(session->userId(), to)) {
      if (blockRepo_->isOneWayBlockedBy(session->userId(), to)) {
        ack(403, "你已拉黑对方，请先解除");
      } else {
        ack(403, "对方已拒收");
      }
      return;
    }
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
  }  // 文本原位校验结束；图片已在上游预检（目标/限流/权限），此处直接复用 to
  // 自己对话框（to==自己账号）：落库持久化，只回 ack 不回显 msg.recv（发送方即自己）
  if (to == session->userId()) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::string messageId = nextMessageId();
    if (!msgRepo_->saveMessage(messageId, session->userId(), to, content, now,
                                clientMsgId, chatType, kMsgDelivered, 0, msgType,
                                imageMediaId)) {
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
                             clientMsgId, chatType, status, 0, msgType,
                             imageMediaId)) {
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
    {"msgType", msgType},
    // 媒体消息透传 fileId（非媒体消息为空串），URL 由客户端调 media.url 获取
    {"mediaId", imageFileId},
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
  // 多端一致性（v2.2）：自发消息回显到发送者的其他在线端（发起连接不回显），
  // 保证同一账号多设备会话视图一致
  for (auto& t : onlineMgr_.sessions(session->userId())) {
    if (t.get() == session.get()) continue;
    t->reply(makeEnvelopeText("msg.recv", recvData));
  }
}

void AppServer::onOfflineAck(const std::shared_ptr<Session>& session, const json& data) {
  // 客户端确认已收到 msg.offline 中的消息，服务端标记为已投递
  // 数组数量上限：超限直接拒绝，防单包触发海量 DB 更新
  if (data.contains("messageIds") && data["messageIds"].is_array() &&
      data["messageIds"].size() > kMaxOfflineAckIds) {
    session->reply(makeResult("msg.offline_ack_res", 400, "确认数量超限", json::object()));
    return;
  }
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
  // 重查询限流：单次最多 200 条 DB 扫描，防高频刷历史拖垮工作线程
  if (!queryLimiter_->allow(session->userId(), kQueryLimitPerWindow, kQueryWindow)) {
    session->reply(makeResult("chat.history_res", 429, "查询过于频繁，请稍后再试",
                              json::object()));
    return;
  }
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
    arr.push_back(messageToJson(m));
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
  // 多端一致性（v2.2）：已读同步——通知该用户其他在线端刷新未读状态（免 ACK，
  // 丢失靠重登补发与 chat.history 拉取兜底）。
  // 推送门槛（v2.2.1 收紧）：仅当与目标确有单聊往来才推——防数字 userId 经
  // 回显枚举账号（2026-09-27 审计低危）；查询失败按无往来（fail-closed 不泄露）
  if (msgRepo_->hasConversation(session->userId(), from)) {
    std::string fromAccount;
    if (userStore_->accountOf(from, &fromAccount)) {
      json rd = {{"from", fromAccount}, {"readTs", ts}};
      for (auto& t : onlineMgr_.sessions(session->userId())) {
        if (t.get() == session.get()) continue;
        t->reply(makeEnvelopeText("msg.read_notify", rd));
      }
    }
  }
}

void AppServer::onRecall(const std::shared_ptr<Session>& session, const json& data) {
  // 消息撤回（msg.recall）：仅发送者、落库后 kRecallWindowSec 内可撤。
  // 原子校验在存储层单条 UPDATE 完成（存在+权限+时间窗+状态，免 TOCTOU）。
  // 成功回执后扇出 msg.recall_notify：单聊=对端全部在线端 + 发送者其他端；
  // 群聊=除发起端外全部在线成员端。重复撤回幂等回 0 且不重复扇出。
  const char* kRes = "msg.recall_res";
  if (!data.contains("messageId") || !data["messageId"].is_string() ||
      data["messageId"].get<std::string>().empty()) {
    session->reply(makeResult(kRes, 400, "参数错误", json::object()));
    return;
  }
  // 限流：与消息发送同桶（撤回频率远低于发送，不设专桶）
  if (!msgLimiter_->allow(session->userId(), kMaxSendPerWindow, kSendWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    session->reply(makeResult(kRes, 429, "操作过于频繁，请稍后再试", json::object()));
    return;
  }
  const std::string messageId = data["messageId"].get<std::string>();
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  RecallInfo info;  // RecallResult/RecallInfo 为命名空间级类型，用非限定名
  const auto r =
      msgRepo_->recallMessage(messageId, session->userId(), now, config::kRecallWindowSec, &info);
  switch (r) {
    case RecallResult::kOk:
      break;
    case RecallResult::kAlready:  // 幂等成功（首次已扇出）
      session->reply(makeResult(kRes, 0, "ok", json::object()));
      return;
    case RecallResult::kNotFound:
      session->reply(makeResult(kRes, 404, "消息不存在", json::object()));
      return;
    case RecallResult::kForbidden:
      session->reply(makeResult(kRes, 403, "无权撤回该消息", json::object()));
      return;
    case RecallResult::kExpired:
      session->reply(makeResult(kRes, 410, "超过可撤回时间", json::object()));
      return;
    case RecallResult::kError:
      session->reply(makeResult(kRes, 500, "服务器内部错误", json::object()));
      return;
  }
  session->reply(makeResult(kRes, 0, "ok", json::object()));
  // notify 扇出：先回执后推送（与 onSend ack-then-deliver 同序）；
  // 回查失败/信息缺失时静默跳过——撤回已生效，同步缺失由占位符兜底
  // （与 msg.read_notify 免 ACK 同先例）
  if (info.chatType.empty()) return;
  std::string fromAccount;
  if (!userStore_->accountOf(session->userId(), &fromAccount)) fromAccount.clear();
  json nd = {{"messageId", messageId}, {"chatType", info.chatType},
             {"from", fromAccount}};
  if (info.chatType == "group") {
    if (info.groupId <= 0) return;
    nd["groupId"] = info.groupId;
    for (const int64_t uid : groupRepo_->listMemberIds(info.groupId)) {
      for (auto& t : onlineMgr_.sessions(uid)) {
        if (t.get() == session.get()) continue;  // 发起端不收（发送者其他端仍收）
        t->reply(makeEnvelopeText("msg.recall_notify", nd));
      }
    }
    return;
  }
  // 单聊：对端全部在线端 + 发送者其他端；自己对话框（to==自己）只发其他端
  if (info.toUser != session->userId()) {
    for (auto& t : onlineMgr_.sessions(info.toUser)) {
      t->reply(makeEnvelopeText("msg.recall_notify", nd));
    }
  }
  for (auto& t : onlineMgr_.sessions(session->userId())) {
    if (t.get() == session.get()) continue;
    t->reply(makeEnvelopeText("msg.recall_notify", nd));
  }
}

void AppServer::onConversationList(const std::shared_ptr<Session>& session, const json& data) {
  // 会话列表（v3.2，派生式）：单聊未读/已读游标出自 messages.status 事实源；
  // 群条目 unreadCount/lastReadTs 恒 0——last_delivered_ts 是投递水位线
  // （在线投递即推进），不是已读游标，语义不可混用；群已读为独立能力
  // （group_members.last_read_ts + group.read）v3.x 再启用（协议字段细则占位）。
  // 流程：单聊聚合 + 群末条 ts 聚合 → 合并按 lastMsgTs 倒序截 limit →
  // 幸存者逐个末条点查（有界 N+1，(ts,id) 严格序防同秒边界）→ 装配展示字段。
  if (!queryLimiter_->allow(session->userId(), kQueryLimitPerWindow, kQueryWindow)) {
    session->reply(makeResult("conversation.list_res", 429, "查询过于频繁，请稍后再试",
                              json::object()));
    return;
  }
  size_t limit = data.value("limit", static_cast<int64_t>(config::kConversationLimitDefault));
  if (limit == 0) limit = config::kConversationLimitDefault;
  if (limit > config::kConversationLimitMax) limit = config::kConversationLimitMax;
  const int64_t me = session->userId();

  struct Cand {
    bool isGroup;
    int64_t id;         // single=对端 userId；group=groupId
    int64_t lastTs;
    int64_t unread;
    int64_t lastReadTs;
  };
  std::vector<Cand> cands;
  // 单聊：repo 已合并收发双方向、排除自己对话框（from/to=me）
  for (const auto& e : msgRepo_->fetchSingleConversations(me)) {
    cands.push_back({false, e.peerId, e.lastMsgTs, e.unread, e.lastReadTs});
  }
  // 群：现群 × 末条 ts 一次聚合（无消息的群不出现；已解散群过滤）
  const auto gids = groupRepo_->listUserGroups(me);
  std::vector<int64_t> alive;
  for (const int64_t gid : gids) {
    bool dissolved = false;
    if (!groupRepo_->getGroup(gid, nullptr, nullptr, nullptr, nullptr, &dissolved)) continue;
    if (!dissolved) alive.push_back(gid);
  }
  for (const auto& [gid, lastTs] : msgRepo_->fetchGroupLastTs(alive)) {
    cands.push_back({true, gid, lastTs, 0, 0});
  }
  std::sort(cands.begin(), cands.end(),
            [](const Cand& a, const Cand& b) { return a.lastTs > b.lastTs; });
  if (cands.size() > limit) cands.resize(limit);

  json arr = json::array();
  for (const auto& c : cands) {
    LastMessageInfo lm;
    const bool has = c.isGroup ? msgRepo_->fetchLastMessageGroup(c.id, &lm)
                               : msgRepo_->fetchLastMessageSingle(me, c.id, &lm);
    if (!has) continue;  // 末条缺失（理论不可达）：防御跳过
    json item = {
      {"chatType", c.isGroup ? "group" : "single"},
      {"lastMessageId", lm.messageId},
      {"lastMsgTs", c.lastTs},
      {"lastMsgType", lm.msgType},
      {"lastMsgRecalled", lm.recalled},
      {"unreadCount", c.unread},
      {"lastReadTs", c.lastReadTs},
    };
    if (c.isGroup) {
      item["groupId"] = c.id;
      std::string gname;
      groupRepo_->getGroup(c.id, &gname, nullptr);
      item["groupName"] = gname;
    } else {
      std::string acct;
      if (!userStore_->accountOf(c.id, &acct)) acct.clear();
      std::string nick;
      userStore_->nickname(c.id, &nick);
      item["account"] = acct;
      item["nick"] = nick;
    }
    arr.push_back(item);
  }
  session->reply(makeResult("conversation.list_res", 0, "ok",
                            json{{"conversations", arr}}));
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
