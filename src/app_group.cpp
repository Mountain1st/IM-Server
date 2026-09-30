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
#include "session.hpp"

#include <ctime>

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
  auto stored = msgRepo_->fetchGroupMessages(groupId, joined - 1, left, limit, false);
  std::string gname;
  groupRepo_->getGroup(groupId, &gname, nullptr);
  json arr = json::array();
  for (const auto& m : stored) {
    std::string fromNick, fromAccount;
    userStore_->nickname(m.fromUser, &fromNick);
    if (!userStore_->accountOf(m.fromUser, &fromAccount)) fromAccount.clear();
    arr.push_back(json{{"from", fromAccount},
                       {"fromNick", fromNick},
                       {"groupId", groupId},
                       {"groupName", gname},
                       {"content", m.content},
                       {"ts", m.ts},
                       {"clientMsgId", m.clientMsgId},
                       {"messageId", m.messageId},
                       {"chatType", "group"}});
  }
  session->reply(makeResult("group.history_res", 0, "ok", json{{"msgs", arr}}));
}

void AppServer::onGroupSend(const std::shared_ptr<Session>& session, const json& data,
  // 群发（onSend 按 chatType=group 分流进入，幂等/限流已在上游完成）：
  // 群存在/未解散/发送者是现成员 → 落库（单行 to_user=0, group_id=G）→
  // 逐成员投递：在线端直接推 msg.recv；离线成员按各水位线
  // （watermark）记 pending，重连补发；发送者其他在线端回显（v2.2）
                            const std::string& content, const std::string& clientMsgId) {
  auto ack = [&](int code, const std::string& msg) {
    session->reply(makeEnvelopeText("msg.ack",
        json{{"code", code}, {"msg", msg}, {"clientMsgId", clientMsgId}}));
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
  if (content.empty()) {
    ack(400, "参数错误");
    return;
  }
  if (content.size() > kMaxContentBytes) {
    ack(413, "消息内容超长（超过64KB）");
    return;
  }
  // 幂等 + 限流（与单聊同一套，按发送方去重）
  if (msgDedup_->isDuplicate(session->userId(), clientMsgId)) {
    ack(0, "ok");
    return;
  }
  const size_t sendLimit = clientMsgId.empty() ? kMaxSendNoIdPerWindow : kMaxSendPerWindow;
  if (!msgLimiter_->allow(session->userId(), sendLimit, kSendWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    ack(429, "消息发送过于频繁，请稍后再试");
    return;
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    ack(403, "非群成员");
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string messageId = nextMessageId();
  if (!msgRepo_->saveMessage(messageId, session->userId(), 0, content, now, clientMsgId,
                             "group", kMsgDelivered, groupId)) {
    ack(500, "服务器内部错误");
    return;
  }
  msgDedup_->remember(session->userId(), clientMsgId);
  ack(0, "ok");
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
                   {"chatType", "group"}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    bool pushed = false;
    for (auto& t : onlineMgr_.sessions(uid)) {
      // 发送者仅发起连接不回显；其其他端回显（v2.2 多端一致），接收方全端投递
      if (uid == session->userId() && t.get() == session.get()) continue;
      t->reply(makeEnvelopeText("msg.recv", recvData));
      pushed = true;
    }
    if (pushed) {
      groupRepo_->setWatermark(groupId, uid, now);
      g_metrics.messagesDelivered.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_metrics.messagesOffline.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

}  // namespace im
