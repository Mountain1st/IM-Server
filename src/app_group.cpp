// ============================================================
// 业务域：群聊全生命周期（group.*，v1.9 建群 → v2.0 管理闭环）
// 创建/邀请/退群 + 管理（踢人/转让/解散/改名/公告）+ 查询（列表/成员/历史）
// - 解散防线：dissolved 标记后除 group.history 外全部群操作回 404；
//   已解散群不进 group.list；成员可拉历史（到解散为止）
// - 权限模型：只有群主可管理（无管理员角色，见协议 §11.3）
// - 成员上限 50：invite 超限回 403 群已满
// - 群事件（removed/owner_changed/dissolved/renamed/announced）仅推在线端，
//   离线不补发（客户端拉列表自愈，已知取舍）
// - onGroupSend 由 onSend 按 chatType=group 分流进入
// 线程：工作线程池（AppServer::onEnvelope 分发）
// ============================================================
#include "appserver.hpp"
#include "app_util.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "server_config.hpp"
#include "session.hpp"

#include <ctime>
#include <unordered_set>

namespace im {

namespace {
constexpr size_t kMaxGroupMembers = 50;      // 群人数上限（含群主）
constexpr size_t kMaxGroupNameLen = 32;      // 群名长度上限
constexpr size_t kMaxAnnouncementBytes = 1024;  // 群公告长度上限
}  // namespace

void AppServer::onGroupCreate(const std::shared_ptr<Session>& session, const json& data) {
  std::string name = data.value("name", "");
  if (name.empty() || name.size() > kMaxGroupNameLen) {
    session->reply(makeResult("group.create_res", 400, "参数错误", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int64_t gid = groupRepo_->createGroup(name, session->userId(), now);
  if (gid <= 0) {
    session->reply(makeResult("group.create_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("group.create_res", 0, "ok",
                            json{{"groupId", gid}, {"name", name}}));
}

void AppServer::onGroupInvite(const std::shared_ptr<Session>& session, const json& data) {
  // 拉人：仅群主可拉。目标须存在且不在群（重入回 409 已在群）；
  // 满员（50）回 403 群已满。新成员收 group.invited，
  // 原成员收 member_changed(join)
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.invite_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult("group.invite_res", 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult("group.invite_res", 404, "群已解散", json::object()));
    return;
  }
  if (ownerId != session->userId()) {
    session->reply(makeResult("group.invite_res", 403, "仅群主可拉人", json::object()));
    return;
  }
  const std::string account = data.value("account", "");
  int64_t target = 0;
  std::string nick;
  const int frc = userStore_->lookupAccount(account, &target, &nick);
  if (frc == 2) {
    session->reply(makeResult("group.invite_res", 500, "服务器内部错误", json::object()));
    return;
  }
  if (frc != 0) {
    session->reply(makeResult("group.invite_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (!contactRepo_->isContact(session->userId(), target)) {
    session->reply(makeResult("group.invite_res", 409, "对方不是你的联系人", json::object()));
    return;
  }
  if (groupRepo_->isMember(groupId, target)) {
    session->reply(makeResult("group.invite_res", 409, "对方已在群中", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  // 满员校验与插入原子完成（B4 自检三轮）：此前 activeCount 与 addMember
  // 两个锁区间存在 TOCTOU——群主多端并发拉人可双双读到 49 都插入，
  // 上限被击穿到 51。返回 1=已满 403、2=DB 错误 500
  const int arc = groupRepo_->addMemberCapped(groupId, target, now, kMaxGroupMembers);
  if (arc == 1) {
    session->reply(makeResult("group.invite_res", 403, "群已满", json::object()));
    return;
  }
  if (arc != 0) {
    session->reply(makeResult("group.invite_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("group.invite_res", 0, "ok", json::object()));
  // 推送被拉人（在线）；离线靠 group.list + msg.offline 自洽
  std::string inviterAccount;
  userStore_->accountOf(session->userId(), &inviterAccount);
  json added = {{"groupId", groupId}, {"name", name}, {"inviter", inviterAccount}};
  for (auto& t : onlineMgr_.sessions(target)) {
    t->reply(makeEnvelopeText("group.added", added));
  }
  // 通知群内在线成员
  std::string targetAccount;
  userStore_->accountOf(target, &targetAccount);
  json changed = {{"groupId", groupId}, {"change", "join"}, {"account", targetAccount}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    if (uid == target) continue;
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.member_changed", changed));
    }
  }
}

void AppServer::onGroupLeave(const std::shared_ptr<Session>& session, const json& data) {
  // 退群：成员自助（群主不能退——须转让或解散）。left_ts=now 行保留
  // （窗口语义：重拉可入群且历史可见到退群为止），自身收 leave_res，
  // 其余在线成员收 member_changed(leave)
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.leave_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId)) {
    session->reply(makeResult("group.leave_res", 404, "群不存在", json::object()));
    return;
  }
  if (ownerId == session->userId()) {
    session->reply(makeResult("group.leave_res", 403, "群主不能退群", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    session->reply(makeResult("group.leave_res", 403, "非群成员", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (!groupRepo_->removeMember(groupId, session->userId(), now)) {
    session->reply(makeResult("group.leave_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("group.leave_res", 0, "ok", json::object()));
  // 通知剩余在线成员
  std::string account;
  userStore_->accountOf(session->userId(), &account);
  json changed = {{"groupId", groupId}, {"change", "leave"}, {"account", account}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.member_changed", changed));
    }
  }
}

void AppServer::onGroupKick(const std::shared_ptr<Session>& session, const json& data) {
  // 踢人：参数解析 → 群存在/未解散 → 仅群主 → 目标存在 →
  // 不能踢群主/须为成员 → removeMember（left_ts 标记，行保留）→
  // 被踢者收 group.removed、其余在线成员收 member_changed(kick)
  // 注意事件不回放（离线成员拉列表自愈，协议 §11.3 已知取舍）
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.kick_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult("group.kick_res", 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult("group.kick_res", 404, "群已解散", json::object()));
    return;
  }
  if (ownerId != session->userId()) {
    session->reply(makeResult("group.kick_res", 403, "仅群主可操作", json::object()));
    return;
  }
  const std::string account = data.value("account", "");
  int64_t target = 0;
  std::string nick;
  const int frc = userStore_->lookupAccount(account, &target, &nick);
  if (frc == 2) {
    session->reply(makeResult("group.kick_res", 500, "服务器内部错误", json::object()));
    return;
  }
  if (frc != 0) {
    session->reply(makeResult("group.kick_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (target == ownerId) {
    session->reply(makeResult("group.kick_res", 409, "不能踢群主", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, target)) {
    session->reply(makeResult("group.kick_res", 409, "目标非群成员", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  // 同退群：left_ts=now，窗口保留，可重拉；失败须回 500（避免缓存/DB 状态分叉还照发事件）
  if (!groupRepo_->removeMember(groupId, target, now)) {
    session->reply(makeResult("group.kick_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("group.kick_res", 0, "ok", json::object()));
  // 被踢者（在线）收 group.removed，其余在线成员收 member_changed(kick)
  json removed = {{"groupId", groupId}, {"name", name}};
  for (auto& t : onlineMgr_.sessions(target)) {
    t->reply(makeEnvelopeText("group.removed", removed));
  }
  std::string targetAccount;
  userStore_->accountOf(target, &targetAccount);
  json changed = {{"groupId", groupId}, {"change", "kick"}, {"account", targetAccount}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.member_changed", changed));
    }
  }
}

void AppServer::onGroupTransfer(const std::shared_ptr<Session>& session, const json& data) {
  // 转让群主：校验链同踢人（存在/未解散/仅群主/目标存在/须为成员）。
  // setOwner 内部带 dissolved 守卫：校验与写入间被并发解散时写入失败
  // 回 404，防状态穿透（终审修复项）
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.transfer_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult("group.transfer_res", 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult("group.transfer_res", 404, "群已解散", json::object()));
    return;
  }
  if (ownerId != session->userId()) {
    session->reply(makeResult("group.transfer_res", 403, "仅群主可操作", json::object()));
    return;
  }
  const std::string account = data.value("account", "");
  int64_t target = 0;
  std::string nick;
  const int frc = userStore_->lookupAccount(account, &target, &nick);
  if (frc == 2) {
    session->reply(makeResult("group.transfer_res", 500, "服务器内部错误", json::object()));
    return;
  }
  if (frc != 0) {
    session->reply(makeResult("group.transfer_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (target == session->userId()) {
    session->reply(makeResult("group.transfer_res", 409, "不能转让给自己", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, target)) {
    session->reply(makeResult("group.transfer_res", 409, "目标非群成员", json::object()));
    return;
  }
  if (!groupRepo_->setOwner(groupId, target)) {
    session->reply(makeResult("group.transfer_res", 404, "群已解散", json::object()));
    return;
  }
  session->reply(makeResult("group.transfer_res", 0, "ok", json::object()));
  // 通知在线成员新群主
  std::string targetAccount;
  userStore_->accountOf(target, &targetAccount);
  json changed = {{"groupId", groupId}, {"account", targetAccount}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.owner_changed", changed));
    }
  }
}

void AppServer::onGroupDissolve(const std::shared_ptr<Session>& session, const json& data) {
  // 解散群：仅群主。置 dissolved 标记（行保留，历史可查到解散为止）。
  // 解散后除 group.history 外全部群操作回 404（6 路防线，
  // 详见 docs/protocol.md v2.0）
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.dissolve_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult("group.dissolve_res", 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult("group.dissolve_res", 404, "群已解散", json::object()));
    return;
  }
  if (ownerId != session->userId()) {
    session->reply(makeResult("group.dissolve_res", 403, "仅群主可操作", json::object()));
    return;
  }
  // 解散前取现成员（推送 dissolved 事件用）
  const auto memberIds = groupRepo_->listMemberIds(groupId);
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (!groupRepo_->dissolveGroup(groupId, now)) {
    session->reply(makeResult("group.dissolve_res", 404, "群已解散", json::object()));
    return;
  }
  session->reply(makeResult("group.dissolve_res", 0, "ok", json::object()));
  // 通知解散前在线成员
  json evt = {{"groupId", groupId}};
  for (const int64_t uid : memberIds) {
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.dissolved", evt));
    }
  }
}

void AppServer::onGroupRename(const std::shared_ptr<Session>& session, const json& data) {
  // 改名：复用建群规则（空或 >32 → 400），仅群主可操作
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.rename_res", 400, "参数错误", json::object()));
    return;
  }
  const std::string name = data.value("name", "");
  if (name.empty() || name.size() > kMaxGroupNameLen) {
    session->reply(makeResult("group.rename_res", 400, "参数错误", json::object()));
    return;
  }
  std::string oldName;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &oldName, &ownerId, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult("group.rename_res", 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult("group.rename_res", 404, "群已解散", json::object()));
    return;
  }
  if (ownerId != session->userId()) {
    session->reply(makeResult("group.rename_res", 403, "仅群主可操作", json::object()));
    return;
  }
  if (!groupRepo_->renameGroup(groupId, name)) {
    session->reply(makeResult("group.rename_res", 404, "群不存在", json::object()));
    return;
  }
  session->reply(makeResult("group.rename_res", 0, "ok", json::object()));
  // 通知在线成员新群名
  json renamed = {{"groupId", groupId}, {"name", name}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.renamed", renamed));
    }
  }
}

void AppServer::onGroupAnnounce(const std::shared_ptr<Session>& session, const json& data) {
  // 公告：空 content=清空（announce_ts 照写），超 1024 字节 → 413，仅群主可操作
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.announce_res", 400, "参数错误", json::object()));
    return;
  }
  const std::string content = data.value("content", "");
  if (content.size() > kMaxAnnouncementBytes) {
    session->reply(makeResult("group.announce_res", 413, "消息内容超长", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult("group.announce_res", 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult("group.announce_res", 404, "群已解散", json::object()));
    return;
  }
  if (ownerId != session->userId()) {
    session->reply(makeResult("group.announce_res", 403, "仅群主可操作", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (!groupRepo_->setAnnouncement(groupId, content, now)) {
    session->reply(makeResult("group.announce_res", 404, "群不存在", json::object()));
    return;
  }
  session->reply(makeResult("group.announce_res", 0, "ok", json::object()));
  // 正文走 list 拉取，不扇出大包，只推 groupId
  json announced = {{"groupId", groupId}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.announced", announced));
    }
  }
}

void AppServer::onGroupList(const std::shared_ptr<Session>& session, const json& data) {
  const auto gids = groupRepo_->listUserGroups(session->userId());
  json arr = json::array();
  for (const int64_t gid : gids) {
    std::string name;
    int64_t ownerId = 0;
    std::string announcement;
    int64_t announceTs = 0;
    bool dissolved = false;
    if (!groupRepo_->getGroup(gid, &name, &ownerId, &announcement, &announceTs, &dissolved))
      continue;
    if (dissolved) continue;
    arr.push_back(json{{"groupId", gid},
                       {"name", name},
                       {"memberCount", groupRepo_->activeCount(gid)},
                       {"isOwner", ownerId == session->userId()},
                       {"announcement", announcement},
                       {"announceTs", announceTs}});
  }
  session->reply(makeResult("group.list_res", 0, "ok", json{{"groups", arr}}));
}

void AppServer::onGroupMembers(const std::shared_ptr<Session>& session, const json& data) {
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.members_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult("group.members_res", 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult("group.members_res", 404, "群已解散", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    session->reply(makeResult("group.members_res", 403, "非群成员", json::object()));
    return;
  }
  // 重查询限流：成员列表为逐成员查询（重），与历史共用额度桶
  if (!queryLimiter_->allow(session->userId(), kQueryLimitPerWindow, kQueryWindow)) {
    session->reply(makeResult("group.members_res", 429, "查询过于频繁，请稍后再试",
                              json::object()));
    return;
  }
  const auto ids = groupRepo_->listMemberIds(groupId);
  // 批量在线查询（N+1 优化：原循环内逐成员 isOnlinePresence）；ownerId 已在上文查出
  const auto onlineFlags = onlineMgr_.isOnlinePresenceBatch(ids);
  json arr = json::array();
  size_t oi = 0;
  for (const int64_t uid : ids) {
    std::string account, username;
    int64_t joined = 0;
    userStore_->accountOf(uid, &account);
    userStore_->nickname(uid, &username);
    groupRepo_->memberPeriod(groupId, uid, &joined, nullptr);
    arr.push_back(json{{"account", account},
                       {"username", username},
                       {"joinedTs", joined},
                       {"isOwner", uid == ownerId},
                       {"online", onlineFlags[oi++]}});
  }
  session->reply(makeResult("group.members_res", 0, "ok", json{{"members", arr}}));
}

void AppServer::onGroupHistory(const std::shared_ptr<Session>& session, const json& data) {
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.history_res", 400, "参数错误", json::object()));
    return;
  }
  // 重查询限流（与 chat.history 同一额度桶）
  if (!queryLimiter_->allow(session->userId(), kQueryLimitPerWindow, kQueryWindow)) {
    session->reply(makeResult("group.history_res", 429, "查询过于频繁，请稍后再试",
                              json::object()));
    return;
  }
  int64_t joined = 0, left = 0;
  if (!groupRepo_->memberPeriod(groupId, session->userId(), &joined, &left)) {
    session->reply(makeResult("group.history_res", 403, "非群成员", json::object()));
    return;
  }
  int limit = data.value("limit", 50);
  // joined-1：sinceTs 语义为严格大于，传 joined-1 使同秒消息可见（与离线路径一致）
  // joined-1：sinceTs 语义为严格大于（sinceId<0 无 id 约束），传 joined-1 使同秒消息可见
  auto stored = msgRepo_->fetchGroupMessages(groupId, joined - 1, -1, left, limit, false);
  std::string gname;
  groupRepo_->getGroup(groupId, &gname, nullptr);
  json arr = json::array();
  for (const auto& m : stored) {
    arr.push_back(messageToJson(m, gname));
  }
  session->reply(makeResult("group.history_res", 0, "ok", json{{"msgs", arr}}));
}

void AppServer::onGroupOfflineAck(const std::shared_ptr<Session>& session, const json& data) {
  // 群投递确认（v3.3，权威规格）：group.offline_ack{groupId, messageIds[]}。
  // messageIds[] = 从当前投递游标起、客户端已处理的**连续可交付消息前缀**。
  // 服务端严格只推进到连续前缀最远端——不得按最大 (ts,id) 无条件跃迁（否则空洞被跳过）。
  // 账号级语义（任一设备确认即推进）；仅确认制(on)驱动游标，阶段A(off) 下只接收不改事实。
  const char* kRes = "group.offline_ack_res";
  // 准入限流（任务③A）：群 ACK 触发 200 条流式游标扫描，防高频空转 DB（独立 arate: 桶）
  if (!ackLimiter_->allow(session->userId(), kMaxAckPerWindow, kAckWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    session->reply(makeResult(kRes, 429, "操作过于频繁，请稍后再试", json::object()));
    return;
  }
  if (!data.contains("groupId") || !data["groupId"].is_number_integer() ||
      !data.contains("messageIds") || !data["messageIds"].is_array()) {
    session->reply(makeResult(kRes, 400, "参数错误", json::object()));
    return;
  }
  const int64_t groupId = data["groupId"].get<int64_t>();
  if (groupId <= 0 || data["messageIds"].size() > kMaxOfflineAckIds) {
    session->reply(makeResult(kRes, 400, "参数错误", json::object()));
    return;
  }
  std::unordered_set<std::string> acked;
  for (const auto& v : data["messageIds"]) {
    if (v.is_string() && !v.get<std::string>().empty()) acked.insert(v.get<std::string>());
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    session->reply(makeResult(kRes, 403, "非群成员", json::object()));
    return;
  }
  // 阶段A(off)：仅接收，不改投递事实（旧语义水位线由推送/补发维护）
  if (!config::deliveryAckRequired()) {
    session->reply(makeResult(kRes, 0, "ok", json::object()));
    return;
  }
  // 连续前缀推进：从当前游标起按 (ts,id) 有序取群消息流，逐条推进；
  // 自身/已撤回=不可交付，直接越过（不阻塞）；遇到未确认的可交付消息即停（空洞）。
  int64_t cts = 0, cid = 0;
  groupRepo_->getDeliveredCursor(groupId, session->userId(), &cts, &cid);
  auto stream = msgRepo_->fetchGroupMessages(groupId, cts, cid, 0, 200, true);
  int64_t nts = cts, nid = cid;
  for (const auto& m : stream) {
    if (m.fromUser == session->userId() || m.status == kMsgRecalled) {
      nts = m.ts;  // 不可交付：越过，不阻塞游标
      nid = m.id;
      continue;
    }
    if (acked.count(m.messageId) == 0) break;  // 空洞：停止推进（连续前缀结束）
    nts = m.ts;
    nid = m.id;
  }
  if ((nts != cts || nid != cid) &&
      !groupRepo_->advanceDeliveredCursor(groupId, session->userId(), nts, nid)) {
    session->reply(makeResult(kRes, 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult(kRes, 0, "ok", json::object()));
}

void AppServer::onGroupRead(const std::shared_ptr<Session>& session, const json& data) {
  // 群已读（v3.4）：成员级已读游标推进（用户级非设备级）。
  // 语义：messageId = 客户端最后可见消息，服务端反查 (ts,内部id) 严格游标，
  // CAS 只进；免 ACK（状态声明，丢失靠重进会话/列表拉取自愈，与 msg.read 同先例）。
  const char* kRes = "group.read_res";
  // 限流：复用已读桶（rrate:，与 msg.read 同额度；防高频推进空转 DB）
  if (!readLimiter_->allow(session->userId(), kMaxReadPerWindow, kAckWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    session->reply(makeResult(kRes, 429, "操作过于频繁，请稍后再试", json::object()));
    return;
  }
  if (!data.contains("groupId") || !data["groupId"].is_number_integer() ||
      !data.contains("messageId") || !data["messageId"].is_string() ||
      data["messageId"].get<std::string>().empty()) {
    session->reply(makeResult(kRes, 400, "参数错误", json::object()));
    return;
  }
  const int64_t groupId = data["groupId"].get<int64_t>();
  const std::string messageId = data["messageId"].get<std::string>();
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, nullptr, nullptr, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult(kRes, 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult(kRes, 404, "群已解散", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    session->reply(makeResult(kRes, 403, "非群成员", json::object()));
    return;
  }
  // 反查消息 (ts,id) 与所属群：不存在 404；跨群 400（防用别群消息推进本群已读）
  int64_t mts = 0, mid = 0, mgid = 0;
  if (!msgRepo_->fetchMessageCursor(messageId, &mts, &mid, &mgid)) {
    session->reply(makeResult(kRes, 404, "消息不存在", json::object()));
    return;
  }
  if (mgid != groupId) {
    session->reply(makeResult(kRes, 400, "消息不属于该群", json::object()));
    return;
  }
  // 严格只进 CAS；失败=DB 错误（命中 0 行即幂等成功）
  if (!groupRepo_->advanceReadCursor(groupId, session->userId(), mts, mid)) {
    session->reply(makeResult(kRes, 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult(kRes, 0, "ok", json::object()));
  // 多端同步：该用户其他在线端（发起端不收；不向群内其他成员推送）
  json rd = {{"groupId", groupId}, {"readTs", mts}, {"messageId", messageId}};
  for (auto& t : onlineMgr_.sessions(session->userId())) {
    if (t.get() == session.get()) continue;
    t->reply(makeEnvelopeText("group.read_notify", rd));
  }
}

void AppServer::onGroupSend(const std::shared_ptr<Session>& session, const json& data,
  // 群发（onSend 按 chatType=group 分流进入，msgType/幂等/图片管线已在上游完成，
  // content 已是正文或落盘 URL；发送限流在本函数内执行）：
  // 群存在/未解散/发送者是现成员 → 落库（单行 to_user=0, group_id=G）→
  // 逐成员投递：在线端直接推 msg.recv；离线成员按各水位线
  // （watermark）记 pending，重连补发；发送者其他在线端回显（v2.2）
                            const std::string& content, const std::string& clientMsgId,
                            const std::string& msgType, int64_t mediaId) {
  auto ack = [&](int code, const std::string& msg, const std::string& messageId = "") {
    session->reply(makeEnvelopeText("msg.ack",
        json{{"code", code}, {"msg", msg}, {"clientMsgId", clientMsgId},
             {"messageId", messageId}}));
  };
  int64_t groupId = 0;
  if (data.contains("to") && data["to"].is_number_integer()) {
    groupId = data["to"].get<int64_t>();
  } else {
    ack(400, "参数错误");
    return;
  }
  std::string gname;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &gname, &ownerId, nullptr, nullptr, &dissolved)) {
    ack(404, "群不存在");
    return;
  }
  if (dissolved) {
    ack(404, "群已解散");
    return;
  }
  // 文本须非空；图片上游已校验 mediaId（content=fileId，非空）
  if (content.empty()) {
    ack(400, "参数错误");
    return;
  }
  // 文本沿用 64KB 上限；图片 content 为 fileId（短串），不适用文本上限
  if (msgType == "text" && content.size() > kMaxContentBytes) {
    ack(413, "消息内容超长（超过64KB）");
    return;
  }
  // 发送限流（与单聊同一套，按发送方去重；图片上游预检已消费一次额度，
  // 此处跳过以防双重消费；成员/权限只读校验仍执行，失败直接拦截）。
  if (msgType == "text") {
  const size_t sendLimit = clientMsgId.empty() ? kMaxSendNoIdPerWindow : kMaxSendPerWindow;
  if (!msgLimiter_->allow(session->userId(), sendLimit, kSendWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    ack(429, "消息发送过于频繁，请稍后再试");
    return;
  }
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    ack(403, "非群成员");
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string messageId = nextMessageId();
  if (!msgRepo_->saveMessage(messageId, session->userId(), 0, content, now, clientMsgId,
                             "group", kMsgDelivered, groupId, msgType, mediaId)) {
    ack(500, "服务器内部错误");
    return;
  }
  msgDedup_->remember(session->userId(), clientMsgId);
  ack(0, "ok", messageId);
  // 发送方信息（from 为账号，userId 对客户端不透明）
  std::string fromNick, fromAccount;
  userStore_->nickname(session->userId(), &fromNick);
  if (!userStore_->accountOf(session->userId(), &fromAccount)) fromAccount.clear();
  json recvData = {{"from", fromAccount},
                   {"fromNick", fromNick},
                   {"groupId", groupId},
                   {"groupName", gname},
                   {"content", content},
                   {"ts", now},
                   {"clientMsgId", clientMsgId},
                   {"messageId", messageId},
                   {"chatType", "group"},
                   {"msgType", msgType},
                   // 媒体消息透传 fileId（存于 content），非媒体为空串
                   {"mediaId", msgType == "image" ? content : std::string()}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    bool pushed = false;
    for (auto& t : onlineMgr_.sessions(uid)) {
      // 发送者仅发起连接不回显；其其他端回显（v2.2 多端一致），接收方全端投递
      if (uid == session->userId() && t.get() == session.get()) continue;
      t->reply(makeEnvelopeText("msg.recv", recvData));
      pushed = true;
    }
    if (pushed) {
      // v3.3：确认制(on)下在线投递不推进游标（由 group.offline_ack 推进）；
      // 阶段A(off)兼容旧语义：推送即推进水位线。
      if (!config::deliveryAckRequired()) {
        groupRepo_->advanceDeliveredCursor(groupId, uid, now, 0);
      }
      g_metrics.messagesPushed.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_metrics.messagesOffline.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

}  // namespace im
