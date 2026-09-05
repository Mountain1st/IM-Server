#include "contact_repository.hpp"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>

namespace im {

ContactRepository::ContactRepository(const std::string& host, unsigned port,
                                     const std::string& user, const std::string& password,
                                     const std::string& db)
  : client_(host, port, user, password, db) {
  loadContactsCache();
}

void ContactRepository::loadContactsCache() {
  if (!client_.ok()) return;
  MYSQL* c = client_.handle();
  if (mysql_query(c, "SELECT user_id, contact_id FROM contacts")) {
    std::cerr << "[mysql] 加载联系人缓存失败: " << client_.errStr() << std::endl;
    return;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0] && row[1]) {
      contacts_.emplace(std::string(row[0]) + "|" + row[1]);
    }
  }
  mysql_free_result(res);
  std::cout << "[mysql] 已加载 " << contacts_.size() << " 条联系人关系到缓存" << std::endl;
}

bool ContactRepository::isContact(int64_t a, int64_t b) {
  std::lock_guard<std::mutex> lock(mutex_);
  return contacts_.count(std::to_string(a) + "|" + std::to_string(b)) != 0;
}

bool ContactRepository::addContactPair(int64_t a, int64_t b) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  const std::string sql =
      "INSERT IGNORE INTO contacts (user_id, contact_id) VALUES (?, ?), (?, ?)";
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(), sql,
      {std::to_string(a), std::to_string(b), std::to_string(b), std::to_string(a)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 添加联系人失败 errno=" << errNo << std::endl;
    return false;
  }
  mysql_stmt_close(stmt);
  contacts_.emplace(std::to_string(a) + "|" + std::to_string(b));
  contacts_.emplace(std::to_string(b) + "|" + std::to_string(a));
  return true;
}

bool ContactRepository::deleteContactPair(int64_t a, int64_t b) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "DELETE FROM contacts WHERE (user_id = ? AND contact_id = ?) "
      "OR (user_id = ? AND contact_id = ?)",
      {std::to_string(a), std::to_string(b), std::to_string(b), std::to_string(a)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 删除联系人失败 errno=" << errNo << std::endl;
    return false;
  }
  mysql_stmt_close(stmt);
  contacts_.erase(std::to_string(a) + "|" + std::to_string(b));
  contacts_.erase(std::to_string(b) + "|" + std::to_string(a));
  return true;
}

std::vector<int64_t> ContactRepository::listContactIds(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int64_t> out;
  if (!client_.ok()) return out;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "SELECT contact_id FROM contacts WHERE user_id = ?", {std::to_string(userId)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 查询联系人列表失败 errno=" << errNo << std::endl;
    return out;
  }
  int64_t cid = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &cid;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    std::cerr << "[mysql] 联系人列表结果绑定失败: " << client_.errStr() << std::endl;
    mysql_stmt_close(stmt);
    return out;
  }
  while (mysql_stmt_fetch(stmt) == 0) out.push_back(cid);
  mysql_stmt_close(stmt);
  return out;
}

int ContactRepository::addRequest(int64_t from, int64_t to) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "INSERT INTO friend_requests (from_user, to_user, status) VALUES (?, ?, 0)",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    if (errNo == 1062) return 1; // 唯一键冲突：已有请求
    std::cerr << "[mysql] 添加好友请求失败 errno=" << errNo << std::endl;
    return 2;
  }
  mysql_stmt_close(stmt);
  return 0;
}

int ContactRepository::acceptRequest(int64_t to, int64_t from) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "UPDATE friend_requests SET status = 1 WHERE from_user = ? AND to_user = ? AND status = 0",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 接受好友请求失败 errno=" << errNo << std::endl;
    return 2;
  }
  const int affected = static_cast<int>(mysql_stmt_affected_rows(stmt));
  mysql_stmt_close(stmt);
  if (affected <= 0) return 1; // 无 pending 请求
  // 建立联系人关系（双向）
  MYSQL_STMT* s2 = mysqlStmtExec(client_.handle(),
      "INSERT IGNORE INTO contacts (user_id, contact_id) VALUES (?, ?), (?, ?)",
      {std::to_string(to), std::to_string(from), std::to_string(from), std::to_string(to)},
      &errNo);
  if (!s2) {
    std::cerr << "[mysql] 接受请求后建联系人失败 errno=" << errNo << std::endl;
    return 2;
  }
  mysql_stmt_close(s2);
  contacts_.emplace(std::to_string(to) + "|" + std::to_string(from));
  contacts_.emplace(std::to_string(from) + "|" + std::to_string(to));
  return 0;
}

int ContactRepository::rejectRequest(int64_t to, int64_t from) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "UPDATE friend_requests SET status = 2 WHERE from_user = ? AND to_user = ? AND status = 0",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 拒绝好友请求失败 errno=" << errNo << std::endl;
    return 2;
  }
  const int affected = static_cast<int>(mysql_stmt_affected_rows(stmt));
  mysql_stmt_close(stmt);
  return affected > 0 ? 0 : 1;
}

std::vector<ContactRepository::PendingRequest> ContactRepository::listPendingRequests(int64_t to) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PendingRequest> out;
  if (!client_.ok()) return out;
  const std::string sql =
      "SELECT from_user, UNIX_TIMESTAMP(created_at) FROM friend_requests WHERE to_user = " +
      std::to_string(to) + " AND status = 0 ORDER BY id ASC";
  if (mysql_query(client_.handle(), sql.c_str())) {
    std::cerr << "[mysql] 查询好友请求失败: " << client_.errStr() << std::endl;
    return out;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return out;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    PendingRequest pr;
    if (row[0]) pr.fromUser = std::strtoll(row[0], nullptr, 10);
    pr.ts = row[1] ? std::strtoll(row[1], nullptr, 10) : 0;
    out.push_back(pr);
  }
  mysql_free_result(res);
  return out;
}

} // namespace im
