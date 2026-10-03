// ============================================================
// MySqlMessageRepository：消息表存取（messages，单行存单聊与群消息）
// - 单聊：from_user/to_user 定位双方；群消息 to_user=0, group_id=G
// - saveMessage：落库（status=0 未投递）；markAcked 批量标记已投递
// - fetchOffline：某用户 pending 单聊消息（补发源，非破坏性）
// - groupPendingSince：群按每成员水位线补发（过滤自发消息）
// - history/groupHistory：历史拉取（LIMIT 100 封顶）
// - markRead：已读水位（read_ts）
// - purgeExpiredOffline：离线 TTL 清理（秒数换算经 duration_cast，
//   防止 chrono::hours.count() 被当秒的历史单位 bug）
// - purgeAgedHistory：已消费消息保留期清理（默认 90 天）
// - hasConversation：双方是否有单聊往来（read_notify 推送门槛）
// ============================================================
#include "mysql_message_repository.hpp"

#include "log.hpp"
#include "server_config.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <ctime>
#include <iostream>
#include <vector>

namespace im {

namespace {
constexpr int kMsgCols = 12; // id,message_id,from_user,to_user,content,ts,client_msg_id,chat_type,status,group_id,msg_type,media_id

// 从一条 MySQL 行构造 StoredMessage（row[] 对应查询列的字符串表示，
// 数字列也由 mysql_fetch_row 以字符串返回）。
StoredMessage rowToMessage(MYSQL_ROW row, unsigned long* lens) {
  StoredMessage m;
  auto str = [&](int i) { return std::string(row[i], lens[i]); };
  m.id = std::strtoll(row[0], nullptr, 10);
  m.messageId = str(1);
  m.fromUser = std::strtoll(row[2], nullptr, 10);
  m.toUser = std::strtoll(row[3], nullptr, 10);
  m.content = str(4);
  m.ts = std::strtoll(row[5], nullptr, 10);
  m.clientMsgId = str(6);
  m.chatType = str(7);
  m.status = std::atoi(row[8]);
  m.groupId = std::strtoll(row[9], nullptr, 10);
  // 空值回读兜底 text：历史数据/异常空串统一视为文本消息
  if (row[10] == nullptr || lens[10] == 0) {
    m.msgType = "text";
  } else {
    m.msgType = str(10);
    if (m.msgType.empty()) m.msgType = "text";
  }
  // 空/NULL 兜 0（历史数据无 media_id）
  m.mediaId = (row[11] == nullptr) ? 0 : std::strtoll(row[11], nullptr, 10);
  return m;
}
} // namespace

MySqlMessageRepository::MySqlMessageRepository(const std::string& host, unsigned port,
                                               const std::string& user,
                                               const std::string& password,
                                               const std::string& db)
  : client_(host, port, user, password, db) {}

bool MySqlMessageRepository::saveMessage(const std::string& messageId, int64_t fromUser,
                                         int64_t toUser, const std::string& content,
                                         int64_t ts, const std::string& clientMsgId,
                                         const std::string& chatType, int status,
                                         int64_t groupId, const std::string& msgType,
                                         int64_t mediaId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  const std::string sql =
      "INSERT INTO messages (message_id, from_user, to_user, content, ts, "
      "client_msg_id, chat_type, status, group_id, msg_type, media_id) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?)";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql,
      {messageId, std::to_string(fromUser), std::to_string(toUser), content,
       std::to_string(ts), clientMsgId, chatType, std::to_string(status),
       std::to_string(groupId), msgType, std::to_string(mediaId)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 保存消息失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  return true;
}

int64_t MySqlMessageRepository::purgeAgedHistory(int64_t nowSec, int retentionDays) {
  // 保留期清理（历史归档语义）：超期已消费消息删除。
  // pending 不在此列（更短的 7 天 TTL 负责）；群消息无 pending 概念，超期即删。
  if (retentionDays <= 0) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  const int64_t expireBefore =
      nowSec - static_cast<int64_t>(retentionDays) * 86400;
  const std::string sql =
      "DELETE FROM messages WHERE ts < " + std::to_string(expireBefore) +
      " AND (group_id != 0 OR status != " + std::to_string(kMsgOfflinePending) + ")";
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 保留期清理失败: " << client_.errStr());
    return 0;
  }
  return static_cast<int64_t>(mysql_affected_rows(client_.handle()));
}

int64_t MySqlMessageRepository::purgeExpiredOffline(int64_t nowSec) {
  // TTL 清理：删除超期未领取的单聊离线消息（群消息不走 pending 状态，不在此列）。
  // 注意 kOfflineTtl 是 hours 单位，须显式换算为秒（历史 bug：直接 .count() 把
  // 168 小时当 168 秒用，实际 TTL 缩短 3600 倍）
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  const int64_t ttlSec = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(config::kOfflineTtl).count());
  const int64_t expireBefore = nowSec - ttlSec;
  const std::string sql =
      "DELETE FROM messages WHERE status = " + std::to_string(kMsgOfflinePending) +
      " AND group_id = 0 AND ts < " + std::to_string(expireBefore);
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 清理过期离线消息失败: " << client_.errStr());
    return 0;
  }
  return static_cast<int64_t>(mysql_affected_rows(client_.handle()));
}

std::vector<StoredMessage> MySqlMessageRepository::fetchOffline(int64_t toUser) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoredMessage> out;
  if (!client_.ok()) return out;
  const std::string sql =
      "SELECT id, message_id, from_user, to_user, content, ts, client_msg_id, chat_type, status, "
      "group_id, msg_type, media_id FROM messages WHERE to_user = " + std::to_string(toUser) +
      " AND status = " + std::to_string(kMsgOfflinePending) +
      " AND group_id = 0 ORDER BY ts ASC, id ASC LIMIT " +
      std::to_string(config::kOfflineMaxPerUser);
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 查询离线消息失败: " << client_.errStr());
    return out;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return out;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    unsigned long* lens = mysql_fetch_lengths(res);
    out.push_back(rowToMessage(row, lens));
  }
  mysql_free_result(res);
  return out;
}

int64_t MySqlMessageRepository::markDelivered(int64_t toUser,
                                              const std::vector<std::string>& messageIds) {
  // 任务③B：单条有界批量 UPDATE（原逐条循环在 1000 条 ACK 时持锁执行 1000 次 SQL）。
  // 谓词不变（to_user + status=0 + IN），已 delivered/recalled/不存在的 id 命中 0 行
  // 幂等安全；返回 affected_rows=本轮真实迁移数（指标只计真实确认）。
  // 批量上限由 handler 层 kMaxOfflineAckIds=1000 保证；此处再兜底截断防直调绕过。
  if (messageIds.empty()) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  const size_t n = std::min(messageIds.size(), config::kMaxOfflineAckIds);
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  // 参数化 IN 列表：每个 id 一个 ?（预处理防注入），非字符串拼接
  std::string sql =
      "UPDATE messages SET status = ?, delivered_ts = ? WHERE to_user = ? AND status = ? "
      "AND message_id IN (";
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) sql += ",";
    sql += "?";
  }
  sql += ")";
  std::vector<std::string> params;
  params.reserve(n + 4);
  params.push_back(std::to_string(kMsgDelivered));
  params.push_back(std::to_string(now));
  params.push_back(std::to_string(toUser));
  params.push_back(std::to_string(kMsgOfflinePending));
  for (size_t i = 0; i < n; ++i) params.push_back(messageIds[i]);
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql, params, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 批量标记已投递失败 errno=" << errNo);
    return 0;
  }
  const int64_t confirmed = static_cast<int64_t>(mysql_stmt_affected_rows(stmt));
  mysql_stmt_close(stmt);
  return confirmed;
}

std::vector<StoredMessage> MySqlMessageRepository::fetchHistory(int64_t userA,
                                                                int64_t userB,
                                                                int limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoredMessage> out;
  if (!client_.ok()) return out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  const std::string sql =
      "SELECT id, message_id, from_user, to_user, content, ts, client_msg_id, chat_type, status, "
      "group_id, msg_type, media_id FROM messages WHERE ((from_user = " + std::to_string(userA) +
      " AND to_user = " + std::to_string(userB) + ") OR (from_user = " +
      std::to_string(userB) + " AND to_user = " + std::to_string(userA) +
      ")) AND group_id = 0 ORDER BY ts DESC, id DESC LIMIT " + std::to_string(limit);
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 查询历史失败: " << client_.errStr());
    return out;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return out;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    unsigned long* lens = mysql_fetch_lengths(res);
    out.push_back(rowToMessage(row, lens));
  }
  mysql_free_result(res);
  return out;
}

bool MySqlMessageRepository::hasConversation(int64_t userA, int64_t userB) {
  // 任一方向存在单聊记录即视为有往来（群消息不算：群内非联系人互见属正常语义）
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || userA <= 0 || userB <= 0) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT 1 FROM messages WHERE group_id = 0 AND "
      "((from_user = ? AND to_user = ?) OR (from_user = ? AND to_user = ?)) LIMIT 1",
      {std::to_string(userA), std::to_string(userB),
       std::to_string(userB), std::to_string(userA)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 会话关系查询失败 errno=" << errNo);
    return false; // 查询失败按无往来处理（fail-closed，不泄露）
  }
  MYSQL_BIND res[1];
  int dummy = 0;
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONG;
  res[0].buffer = &dummy;
  bool found = false;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
      mysql_stmt_fetch(stmt) == 0) {
    found = true;
  }
  mysql_stmt_close(stmt);
  return found;
}

bool MySqlMessageRepository::hasMediaAccess(int64_t userId, const std::string& fileId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || userId <= 0 || fileId.empty()) return false;
  int errNo = 0;
  // 单聊可达（from/to=userId）或群聊可达（userId 为引用该媒体的群消息的现成员）。
  // media_id 以子查询按 file_id 映射；EXISTS 命中即返回，避免排序临时表。
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT EXISTS(SELECT 1 FROM messages WHERE media_id="
      "(SELECT id FROM media WHERE file_id=?) "
      "AND status != " + std::to_string(kMsgRecalled) + " "
      "AND ((group_id=0 AND (from_user=? OR to_user=?)) "
      "OR (group_id>0 AND EXISTS(SELECT 1 FROM group_members gm WHERE "
      "gm.group_id=messages.group_id AND gm.user_id=? AND gm.left_ts=0))))",
      {fileId, std::to_string(userId), std::to_string(userId),
       std::to_string(userId)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 媒体访问查询失败 errno=" << errNo);
    return false; // 查询失败 fail-closed
  }
  MYSQL_BIND res[1];
  int found = 0;
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONG;
  res[0].buffer = &found;
  bool allow = false;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 媒体访问结果读取失败: " << client_.errStr());
  } else if (mysql_stmt_fetch(stmt) == 0) {
    allow = (found == 1);
  }
  mysql_stmt_close(stmt);
  return allow;
}

void MySqlMessageRepository::markRead(int64_t toUser, int64_t fromUser, int64_t readTs) {  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE messages SET status = ?, read_ts = ? "
      "WHERE to_user = ? AND from_user = ? AND status IN (?, ?) AND ts <= ? AND group_id = 0",
      {std::to_string(kMsgRead), std::to_string(readTs), std::to_string(toUser),
       std::to_string(fromUser), std::to_string(kMsgOfflinePending),
       std::to_string(kMsgDelivered), std::to_string(readTs)},
      &errNo);
  if (stmt) mysql_stmt_close(stmt);
  else LOG_E("[mysql] 标记已读失败 errno=" << errNo);
}

RecallResult MySqlMessageRepository::recallMessage(
    const std::string& messageId, int64_t senderId, int64_t nowSec,
    int64_t windowSec, RecallInfo* info) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || messageId.empty() || senderId <= 0 || windowSec <= 0) {
    return RecallResult::kError;
  }
  // 单条 UPDATE 原子校验：存在性 + 权限 + 时间窗 + 状态迁移（免 TOCTOU）
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE messages SET status = ?, recalled_ts = ? "
      "WHERE message_id = ? AND from_user = ? AND status IN (?, ?, ?) AND ts >= ?",
      {std::to_string(kMsgRecalled), std::to_string(nowSec), messageId,
       std::to_string(senderId), std::to_string(kMsgOfflinePending),
       std::to_string(kMsgDelivered), std::to_string(kMsgRead),
       std::to_string(nowSec - windowSec)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 撤回消息失败 errno=" << errNo);
    return RecallResult::kError;
  }
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  if (affected != 1) {
    // 失败分类：回查该行（仅失败路径；chatType/groupId/to_user 均不可变，
    // 与 UPDATE 之间无 TOCTOU 风险）
    errNo = 0;
    MYSQL_STMT* sel = client_.stmtExec(
        "SELECT from_user, status, ts FROM messages WHERE message_id = ?",
        {messageId}, &errNo);
    if (!sel) {
      LOG_E("[mysql] 撤回分类查询失败 errno=" << errNo);
      return RecallResult::kError;
    }
    MYSQL_BIND res[3];
    int64_t fromUser = 0, ts = 0;
    int status = 0;
    memset(res, 0, sizeof res);
    res[0].buffer_type = MYSQL_TYPE_LONGLONG;
    res[0].buffer = &fromUser;
    res[1].buffer_type = MYSQL_TYPE_LONG;
    res[1].buffer = &status;
    res[2].buffer_type = MYSQL_TYPE_LONGLONG;
    res[2].buffer = &ts;
    const bool hasRow =
        !mysql_stmt_bind_result(sel, res) && !mysql_stmt_store_result(sel) &&
        mysql_stmt_fetch(sel) == 0;
    mysql_stmt_close(sel);
    if (!hasRow) return RecallResult::kNotFound;
    if (fromUser != senderId) return RecallResult::kForbidden;
    if (status == kMsgRecalled) return RecallResult::kAlready;
    if (ts < nowSec - windowSec) return RecallResult::kExpired;
    return RecallResult::kError;  // 与 UPDATE 条件相符却未命中：并发删除等异常
  }
  // 成功：回查行信息供 notify 扇出（chat_type/group_id/to_user 不可变）
  if (info) {
    errNo = 0;
    MYSQL_STMT* sel = client_.stmtExec(
        "SELECT chat_type, group_id, to_user FROM messages WHERE message_id = ?",
        {messageId}, &errNo);
    if (!sel) {
      LOG_E("[mysql] 撤回信息回查失败 errno=" << errNo);
      info->chatType = "single";  // 回查失败降级：notify 缺失可由占位符兜底
      return RecallResult::kOk;
    }
    MYSQL_BIND res[3];
    char chatBuf[16] = {0};
    unsigned long chatLen = 0;
    int64_t groupId = 0, toUser = 0;
    memset(res, 0, sizeof res);
    res[0].buffer_type = MYSQL_TYPE_STRING;
    res[0].buffer = chatBuf;
    res[0].buffer_length = sizeof chatBuf;
    res[0].length = &chatLen;
    res[1].buffer_type = MYSQL_TYPE_LONGLONG;
    res[1].buffer = &groupId;
    res[2].buffer_type = MYSQL_TYPE_LONGLONG;
    res[2].buffer = &toUser;
    if (!mysql_stmt_bind_result(sel, res) && !mysql_stmt_store_result(sel) &&
        mysql_stmt_fetch(sel) == 0) {
      info->chatType.assign(chatBuf, chatLen);
      info->groupId = groupId;
      info->toUser = toUser;
    } else {
      info->chatType = "single";  // 降级同上
    }
    mysql_stmt_close(sel);
  }
  return RecallResult::kOk;
}

std::vector<StoredMessage> MySqlMessageRepository::fetchGroupMessages(
    int64_t groupId, int64_t sinceTs, int64_t sinceId, int64_t untilTs, int limit,
    bool asc) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoredMessage> out;
  if (!client_.ok() || groupId <= 0) return out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  // 游标语义：
  //   sinceId >= 0：(ts,id) 严格游标——ts > sinceTs 或 (ts==sinceTs 且 id>sinceId)
  //   sinceId < 0 ：无 id 约束，仅 ts > sinceTs（历史/入群下界：严格大于，使同秒可见）
  // 排序与游标同构 (ts,id)，防“查询排序≠ACK游标排序”边界 bug
  std::string cursorCond;
  if (sinceId < 0) {
    cursorCond = "ts > " + std::to_string(sinceTs);
  } else {
    cursorCond = "(ts > " + std::to_string(sinceTs) + " OR (ts = " +
                 std::to_string(sinceTs) + " AND id > " + std::to_string(sinceId) + "))";
  }
  std::string sql =
      "SELECT id, message_id, from_user, to_user, content, ts, client_msg_id, chat_type, status, "
      "group_id, msg_type, media_id FROM messages WHERE group_id = " +
      std::to_string(groupId) + " AND " + cursorCond;
  if (untilTs > 0) sql += " AND ts <= " + std::to_string(untilTs);
  sql += asc ? " ORDER BY ts ASC, id ASC LIMIT " : " ORDER BY ts DESC, id DESC LIMIT ";
  sql += std::to_string(limit);
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 查询群消息失败: " << client_.errStr());
    return out;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return out;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    unsigned long* lens = mysql_fetch_lengths(res);
    out.push_back(rowToMessage(row, lens));
  }
  mysql_free_result(res);
  return out;
}

bool MySqlMessageRepository::fetchMessageCursor(const std::string& messageId, int64_t* ts,
                                                int64_t* id, int64_t* groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || messageId.empty()) return false;
  int errNo = 0;
  // messageId 为客户端输入 → 预处理语句防注入
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT ts, id, group_id FROM messages WHERE message_id = ? LIMIT 1",
      {messageId}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 反查消息游标失败 errno=" << errNo);
    return false;
  }
  int64_t vts = 0, vid = 0, vgid = 0;
  MYSQL_BIND res[3];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &vts;
  res[1].buffer_type = MYSQL_TYPE_LONGLONG;
  res[1].buffer = &vid;
  res[2].buffer_type = MYSQL_TYPE_LONGLONG;
  res[2].buffer = &vgid;
  const bool ok = !mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
                  mysql_stmt_fetch(stmt) == 0;
  mysql_stmt_close(stmt);
  if (ok) {
    if (ts) *ts = vts;
    if (id) *id = vid;
    if (groupId) *groupId = vgid;
  }
  return ok;
}

std::vector<ConversationEntry> MySqlMessageRepository::fetchSingleConversations(
    int64_t userId) {
  // 单聊会话聚合（v3.2 派生式）：收方向出 unread/lastReadTs/lastTs，
  // 发方向仅补 lastTs；两方向在 C++ 合并。不含自己对话框（群未读语义
  // 见规格——last_delivered_ts 是投递水位线，v3.2 群未读由上层恒 0）。
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ConversationEntry> out;
  if (!client_.ok() || userId <= 0) return out;
  std::map<int64_t, ConversationEntry> merged;
  const std::string me = std::to_string(userId);
  // 收方向（idx_recv_peer）：未读 = SUM(status IN (0,1))，已读游标 = MAX(read_ts)
  const std::string recvSql =
      "SELECT from_user, MAX(ts), SUM(status IN (" +
      std::to_string(kMsgOfflinePending) + ", " + std::to_string(kMsgDelivered) +
      ")), MAX(read_ts) FROM messages WHERE to_user = " + me +
      " AND group_id = 0 AND from_user != " + me + " GROUP BY from_user";
  if (!client_.query(recvSql.c_str())) {
    LOG_E("[mysql] 会话聚合(收)失败: " << client_.errStr());
    return out;
  }
  if (MYSQL_RES* res = mysql_store_result(client_.handle())) {
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
      ConversationEntry e;
      e.peerId = std::atoll(row[0]);
      e.lastMsgTs = std::atoll(row[1]);
      e.unread = row[2] ? std::atoll(row[2]) : 0;
      e.lastReadTs = row[3] ? std::atoll(row[3]) : 0;
      merged.emplace(e.peerId, e);
    }
    mysql_free_result(res);
  }
  // 发方向（idx_pair_ts 前缀）：仅补 lastTs（自己发的消息不产生自己的未读）
  const std::string sentSql =
      "SELECT to_user, MAX(ts) FROM messages WHERE from_user = " + me +
      " AND group_id = 0 AND to_user != " + me + " GROUP BY to_user";
  if (!client_.query(sentSql.c_str())) {
    LOG_E("[mysql] 会话聚合(发)失败: " << client_.errStr());
    return out;
  }
  if (MYSQL_RES* res = mysql_store_result(client_.handle())) {
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
      const int64_t peer = std::atoll(row[0]);
      const int64_t ts = std::atoll(row[1]);
      auto it = merged.find(peer);
      if (it != merged.end()) {
        it->second.lastMsgTs = std::max(it->second.lastMsgTs, ts);
      } else {
        ConversationEntry e;
        e.peerId = peer;
        e.lastMsgTs = ts;
        merged.emplace(peer, e);
      }
    }
    mysql_free_result(res);
  }
  out.reserve(merged.size());
  for (auto& [_, e] : merged) out.push_back(e);
  return out;
}

bool MySqlMessageRepository::fetchLastMessageSingle(int64_t userA, int64_t userB,
                                                    LastMessageInfo* out) {
  // 单聊末条（有界点查）：(ts,id) 严格序，同秒消息取 id 最大（插入序最新）
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || userA <= 0 || userB <= 0 || userA == userB || !out) {
    return false;
  }
  const std::string sql =
      "SELECT message_id, msg_type, status FROM messages WHERE group_id = 0 AND "
      "((from_user = " + std::to_string(userA) + " AND to_user = " + std::to_string(userB) +
      ") OR (from_user = " + std::to_string(userB) + " AND to_user = " + std::to_string(userA) +
      ")) ORDER BY ts DESC, id DESC LIMIT 1";
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 会话末条(单聊)失败: " << client_.errStr());
    return false;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return false;
  MYSQL_ROW row = mysql_fetch_row(res);
  const bool ok = row != nullptr;
  if (ok) {
    out->messageId = row[0] ? row[0] : "";
    out->msgType = row[1] ? row[1] : "text";
    out->recalled = row[2] && std::atoi(row[2]) == kMsgRecalled;
  }
  mysql_free_result(res);
  return ok;
}

bool MySqlMessageRepository::fetchLastMessageGroup(int64_t groupId,
                                                   LastMessageInfo* out) {
  // 群末条（有界点查）：(ts,id) 严格序
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || groupId <= 0 || !out) return false;
  const std::string sql =
      "SELECT message_id, msg_type, status FROM messages WHERE group_id = " +
      std::to_string(groupId) + " ORDER BY ts DESC, id DESC LIMIT 1";
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 会话末条(群)失败: " << client_.errStr());
    return false;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return false;
  MYSQL_ROW row = mysql_fetch_row(res);
  const bool ok = row != nullptr;
  if (ok) {
    out->messageId = row[0] ? row[0] : "";
    out->msgType = row[1] ? row[1] : "text";
    out->recalled = row[2] && std::atoi(row[2]) == kMsgRecalled;
  }
  mysql_free_result(res);
  return ok;
}

int64_t MySqlMessageRepository::countGroupUnread(int64_t groupId, int64_t userId,
                                                 int64_t readTs, int64_t readId,
                                                 int64_t joinedTs,
                                                 int64_t joinedBoundaryId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || groupId <= 0 || userId <= 0) return -1;
  // 未读 = 已读游标之后 ∧ 非自发 ∧ 未撤回 ∧ 严格晚于入群边界 (joinedTs,id)
  // 两处均严格 (ts,id) 序（同秒入群/同秒已读均无歧义）
  const std::string jts = std::to_string(joinedTs);
  const std::string jid = std::to_string(joinedBoundaryId);
  const std::string rts = std::to_string(readTs);
  const std::string rid = std::to_string(readId);
  const std::string sql =
      "SELECT COUNT(*) FROM messages WHERE group_id = " + std::to_string(groupId) +
      " AND from_user != " + std::to_string(userId) +
      " AND status != " + std::to_string(kMsgRecalled) +
      " AND (ts > " + jts + " OR (ts = " + jts + " AND id > " + jid + "))" +
      " AND (ts > " + rts + " OR (ts = " + rts + " AND id > " + rid + "))";
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 群未读计数失败: " << client_.errStr());
    return -1;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return -1;
  int64_t n = -1;
  if (MYSQL_ROW row = mysql_fetch_row(res)) {
    if (row[0]) n = std::atoll(row[0]);
  }
  mysql_free_result(res);
  return n;
}

std::vector<std::pair<int64_t, int64_t>> MySqlMessageRepository::fetchGroupLastTs(
    const std::vector<int64_t>& groupIds) {
  // 各群末条 ts 一次聚合（idx_group_ts）；IN 列表为服务端群 ID（纯数字）拼接
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<int64_t, int64_t>> out;
  if (!client_.ok() || groupIds.empty()) return out;
  std::string in;
  for (const int64_t gid : groupIds) {
    if (gid <= 0) continue;
    if (!in.empty()) in += ",";
    in += std::to_string(gid);
  }
  if (in.empty()) return out;
  const std::string sql =
      "SELECT group_id, MAX(ts) FROM messages WHERE group_id IN (" + in +
      ") GROUP BY group_id";
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 会话聚合(群)失败: " << client_.errStr());
    return out;
  }
  if (MYSQL_RES* res = mysql_store_result(client_.handle())) {
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
      out.emplace_back(std::atoll(row[0]), std::atoll(row[1]));
    }
    mysql_free_result(res);
  }
  return out;
}

} // namespace im