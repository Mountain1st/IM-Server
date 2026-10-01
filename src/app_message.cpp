// ============================================================
// 业务域：消息收发核心（msg.* / chat.*）——服务端最热路径
// - onSend：单聊/群聊统一入口。处理顺序（有依赖，勿调整）：
//   msgType 白名单（text/image，兼容 msg_type，非字符串 400）→ 幂等查询
//   （窗口内同 clientMsgId 直接回 ack 0；空 id 跳过；不消耗限流额度，
//   图片重发不重复限流/落盘）→ image 走图片分支
//   （1.5MB 上限 413 → base64 合法 400 → 图片独立限流 429 → 并发闸 2 →
//   解码落盘 400，content 换 URL 后并入下述 text 流程；ack 不带 msg_type）
//   → 参数校验（to 解析）→ 限流（30 条/10s；空 clientMsgId 更严 5 条）
//   → 权限校验
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
// - pushOfflineMessages：登录/重连后离线消息补发（非破坏性）
// - nextMessageId：全局消息 ID（时间戳前缀 + 进程内自增，防重启碰撞）
// 线程：工作线程池（AppServer::onEnvelope 分发）
// ============================================================
#include "appserver.hpp"
#include "app_util.hpp"
#include "log.hpp"
#include "media_store.hpp"
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
      {"msgType", m.msgType},
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
      if (m.fromUser == userId) continue;  // 跳过发送者自己的消息（与在线路径一致）
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
                         {"chatType", "group"},
                         {"msgType", m.msgType}});
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
  // 放在 msgType 分流之后、图片限流/解码之前：合法重试不消耗图片配额、
  // 不产生孤儿文件；同样在文本限流之前，合法重试不消耗发送配额；
  // 且不受权限状态影响（重发恒回 ack 0）。空 clientMsgId 跳过（不参与去重）。
  if (!clientMsgId.empty() && msgDedup_->isDuplicate(session->userId(), clientMsgId)) {
    ack(0, "ok");
    return;
  }
  if (msgType == "image") {
    // 图片廉价检查在前（解码落盘前，顺序有依赖勿调整）：
    // 空/超 1.5MB → 413；非法 base64 → 400；
    // 按用户图片限流（独立桶 10 张/分钟，被拒不记幂等）→ 429。
    if (content.empty() || content.size() > kMaxImageB64Bytes) {
      ack(413, "图片过大");
      return;
    }
    if (!MediaStore::isValidBase64(content)) {
      ack(400, "参数错误");
      return;
    }
    if (!imgLimiter_->allow(session->userId(), kImagePerWindow, kImageWindow)) {
      g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
      ack(429, "图片发送过于频繁，请稍后再试");
      return;
    }
    // 并发闸（同时最多 2 路解码，RAII 释放）→ 解码校验落盘；
    // 失败 → 400（只记大小不记字节，见 MediaStore 日志）。
    struct ImgDecodePermit {
      AppServer* srv;
      ImgDecodePermit(AppServer* s, std::unique_lock<std::mutex>& lk) : srv(s) {
        srv->imgDecodeCv_.wait(lk, [this] { return srv->imgDecodeAvail_ > 0; });
        --srv->imgDecodeAvail_;
      }
      ~ImgDecodePermit() {
        {
          std::lock_guard<std::mutex> lk(srv->imgDecodeMutex_);
          ++srv->imgDecodeAvail_;
        }
        srv->imgDecodeCv_.notify_one();
      }
    };
    std::string url;
    {
      std::unique_lock<std::mutex> lk(imgDecodeMutex_);
      ImgDecodePermit permit(this, lk);
      lk.unlock();
      auto stored = MediaStore::decodeAndStore(content, uploadsDir_);
      if (!stored) {
        ack(400, "参数错误");
        return;
      }
      url = *stored;
    }
    // 内容替换为落盘 URL 后走落库/投递路径（64KB 文本上限不再适用）
    content = url;
  }
  if (chatType == "group") {
    onGroupSend(session, data, content, clientMsgId, msgType);
    return;
  }
  if (content.empty()) {
    ack(400, "参数错误");
    return;
  } else if (content.size() > kMaxContentBytes) {
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
  // 自己对话框（to==自己账号）：落库持久化，只回 ack 不回显 msg.recv（发送方即自己）
  if (to == session->userId()) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::string messageId = nextMessageId();
    if (!msgRepo_->saveMessage(messageId, session->userId(), to, content, now,
                                clientMsgId, chatType, kMsgDelivered, 0, msgType)) {
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
                             clientMsgId, chatType, status, 0, msgType)) {
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
      {"msgType", m.msgType},
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
