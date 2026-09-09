#include "mysql_message_repository.hpp"

#include "log.hpp"
#include "server_config.hpp"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <vector>

namespace im {

namespace {
constexpr int kMsgCols = 9; // message_id,from_user,to_user,content,ts,client_msg_id,chat_type,status,group_id

// 从一条 MySQL 行构造 StoredMessage（row[] 对应查询列的字符串表示，
// 数字列也由 mysql_fetch_row 以字符串返回）。
StoredMessage rowToMessage(MYSQL_ROW row, unsigned long* lens) {
  StoredMessage m;
  auto str = [&](int i) { return std::string(row[i], lens[i]); };
  m.messageId = str(0);
  m.fromUser = std::strtoll(row[1], nullptr, 10);
  m.toUser = std::strtoll(row[2], nullptr, 10);
  m.content = str(3);
  m.ts = std::strtoll(row[4], nullptr, 10);
  m.clientMsgId = str(5);
  m.chatType = str(6);
  m.status = std::atoi(row[7]);
  m.groupId = std::strtoll(row[8], nullptr, 10);
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
                                         int64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  const std::string sql =
      "INSERT INTO messages (message_id, from_user, to_user, content, ts, "
      "client_msg_id, chat_type, status, group_id) VALUES (?,?,?,?,?,?,?,?,?)";
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(), sql,
      {messageId, std::to_string(fromUser), std::to_string(toUser), content,
       std::to_string(ts), clientMsgId, chatType, std::to_string(status),
       std::to_string(groupId)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 保存消息失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  return true;
}

std::vector<StoredMessage> MySqlMessageRepository::fetchOffline(int64_t toUser) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoredMessage> out;
  if (!client_.ok()) return out;
  const std::string sql =
      "SELECT message_id, from_user, to_user, content, ts, client_msg_id, chat_type, status, "
      "group_id FROM messages WHERE to_user = " + std::to_string(toUser) +
      " AND status = " + std::to_string(kMsgOfflinePending) +
      " AND group_id = 0 ORDER BY ts ASC, id ASC LIMIT " +
      std::to_string(config::kOfflineMaxPerUser);
  if (mysql_query(client_.handle(), sql.c_str())) {
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

void MySqlMessageRepository::markDelivered(int64_t toUser,
                                           const std::vector<std::string>& messageIds) {
  if (messageIds.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int64_t now = static_cast<int64_t>(std::time(nullptr));
  for (const auto& mid : messageIds) {
    int errNo = 0;
    MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
        "UPDATE messages SET status = ?, delivered_ts = ? "
        "WHERE to_user = ? AND message_id = ? AND status = ?",
        {std::to_string(kMsgDelivered), std::to_string(now),
         std::to_string(toUser), mid, std::to_string(kMsgOfflinePending)},
        &errNo);
    if (stmt) mysql_stmt_close(stmt);
    else LOG_E("[mysql] 标记已投递失败 errno=" << errNo);
  }
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
      "SELECT message_id, from_user, to_user, content, ts, client_msg_id, chat_type, status, "
      "group_id FROM messages WHERE ((from_user = " + std::to_string(userA) +
      " AND to_user = " + std::to_string(userB) + ") OR (from_user = " +
      std::to_string(userB) + " AND to_user = " + std::to_string(userA) +
      ")) AND group_id = 0 ORDER BY ts DESC, id DESC LIMIT " + std::to_string(limit);
  if (mysql_query(client_.handle(), sql.c_str())) {
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

void MySqlMessageRepository::markRead(int64_t toUser, int64_t fromUser, int64_t readTs) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "UPDATE messages SET status = ?, read_ts = ? "
      "WHERE to_user = ? AND from_user = ? AND status != ? AND ts <= ? AND group_id = 0",
      {std::to_string(kMsgRead), std::to_string(readTs), std::to_string(toUser),
       std::to_string(fromUser), std::to_string(kMsgRead), std::to_string(readTs)},
      &errNo);
  if (stmt) mysql_stmt_close(stmt);
  else LOG_E("[mysql] 标记已读失败 errno=" << errNo);
}

std::vector<StoredMessage> MySqlMessageRepository::fetchGroupMessages(
    int64_t groupId, int64_t sinceTs, int64_t untilTs, int limit, bool asc) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoredMessage> out;
  if (!client_.ok() || groupId <= 0) return out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  std::string sql =
      "SELECT message_id, from_user, to_user, content, ts, client_msg_id, chat_type, status, "
      "group_id FROM messages WHERE group_id = " + std::to_string(groupId) +
      " AND ts > " + std::to_string(sinceTs);
  if (untilTs > 0) sql += " AND ts <= " + std::to_string(untilTs);
  sql += asc ? " ORDER BY ts ASC, id ASC LIMIT " : " ORDER BY ts DESC, id DESC LIMIT ";
  sql += std::to_string(limit);
  if (mysql_query(client_.handle(), sql.c_str())) {
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

} // namespace im