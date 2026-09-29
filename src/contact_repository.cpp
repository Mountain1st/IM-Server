#include "contact_repository.hpp"

#include "log.hpp"

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
  std::lock_guard<std::mutex> lock(mutex_);
  if (!fetchAllContacts(&contacts_)) return;
  LOG_I("[mysql] 已加载 " << contacts_.size() << " 条联系人关系到缓存");
}

bool ContactRepository::fetchAllContacts(std::unordered_set<std::string>* out) {
  // 调用方须持有 mutex_（构造加载与重建共用同一查询）
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  if (!client_.query("SELECT user_id, contact_id FROM contacts")) {
    LOG_E("[mysql] 查询联系人失败: " << client_.errStr());
    return false;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return false;
  out->clear();
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0] && row[1]) {
      out->emplace(std::string(row[0]) + "|" + row[1]);
    }
  }
  mysql_free_result(res);
  return true;
}

bool ContactRepository::dbContactCount(size_t* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  if (!client_.query("SELECT COUNT(*) FROM contacts")) {
    LOG_E("[mysql] 统计联系人行数失败: " << client_.errStr());
    return false;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return false;
  bool ok = false;
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row && row[0]) {
    char* endp = nullptr;
    const unsigned long long v = std::strtoull(row[0], &endp, 10);
    if (endp && *endp == '\0') {
      *out = static_cast<size_t>(v);
      ok = true;
    }
  }
  mysql_free_result(res);
  return ok;
}

size_t ContactRepository::cacheContactCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return contacts_.size();
}

void ContactRepository::reloadContactsCache() {
  // 先查全量到临时集合，成功才换入：查询失败保持旧缓存，
  // 避免"先清空再加载"在失败时把缓存打成空（isContact 全 false，消息全 403）
  std::unordered_set<std::string> fresh;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fetchAllContacts(&fresh)) return;
    contacts_.swap(fresh);
    LOG_I("[mysql] 联系人缓存已重建（" << contacts_.size() << " 条，触发：计数漂移）");
  }
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
  MYSQL_STMT* stmt = client_.stmtExec(sql,
      {std::to_string(a), std::to_string(b), std::to_string(b), std::to_string(a)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 添加联系人失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  contacts_.emplace(std::to_string(a) + "|" + std::to_string(b));
  contacts_.emplace(std::to_string(b) + "|" + std::to_string(a));
  return true;
}

void ContactRepository::addSelfContact(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "INSERT IGNORE INTO contacts (user_id, contact_id) VALUES (?, ?)",
      {std::to_string(userId), std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 添加自己联系人失败 errno=" << errNo);
    return;
  }
  mysql_stmt_close(stmt);
  contacts_.emplace(std::to_string(userId) + "|" + std::to_string(userId));
}

bool ContactRepository::deleteContactPair(int64_t a, int64_t b) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "DELETE FROM contacts WHERE (user_id = ? AND contact_id = ?) "
      "OR (user_id = ? AND contact_id = ?)",
      {std::to_string(a), std::to_string(b), std::to_string(b), std::to_string(a)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 删除联系人失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  // 解除关系同时将双向好友请求置为 rejected，允许后续重新发起添加
  MYSQL_STMT* req = client_.stmtExec(
      "UPDATE friend_requests SET status = 2 WHERE (from_user = ? AND to_user = ?) "
      "OR (from_user = ? AND to_user = ?)",
      {std::to_string(a), std::to_string(b), std::to_string(b), std::to_string(a)}, &errNo);
  if (req) mysql_stmt_close(req);
  else LOG_E("[mysql] 删除联系人时关闭好友请求失败 errno=" << errNo);
  contacts_.erase(std::to_string(a) + "|" + std::to_string(b));
  contacts_.erase(std::to_string(b) + "|" + std::to_string(a));
  return true;
}

std::vector<int64_t> ContactRepository::listContactIds(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int64_t> out;
  if (!client_.ok()) return out;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT contact_id FROM contacts WHERE user_id = ?", {std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 查询联系人列表失败 errno=" << errNo);
    return out;
  }
  int64_t cid = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &cid;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 联系人列表结果绑定失败: " << client_.errStr());
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
  // 仅当已有行处于 rejected(2) 时重置为 pending(0)（允许重新发起）；
  // pending/accepted 保持原状（409 已存在/已是联系人由上层处理）。
  MYSQL_STMT* stmt = client_.stmtExec(
      "INSERT INTO friend_requests (from_user, to_user, status) VALUES (?, ?, 0) "
      "ON DUPLICATE KEY UPDATE status = IF(status = 2, 0, status)",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 添加好友请求失败 errno=" << errNo);
    return 2;
  }
  // affected_rows 语义：1=新插入成功；0=冲突且未变化（pending/accepted→409）；
  // 2=冲突但被重置为 pending（原 rejected→成功）。
  const int affected = static_cast<int>(mysql_stmt_affected_rows(stmt));
  mysql_stmt_close(stmt);
  return affected == 1 || affected == 2 ? 0 : 1;
}

int ContactRepository::acceptRequest(int64_t to, int64_t from) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE friend_requests SET status = 1 WHERE from_user = ? AND to_user = ? AND status = 0",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 接受好友请求失败 errno=" << errNo);
    return 2;
  }
  const int affected = static_cast<int>(mysql_stmt_affected_rows(stmt));
  mysql_stmt_close(stmt);
  if (affected <= 0) return 1; // 无 pending 请求
  // 反向请求一并置为 accepted：互相添加场景下，双方既已成为联系人，
  // 反向待处理请求不再有意义，避免残留（如 B 接受 A 后 A 列表仍显示 B 的请求）。
  MYSQL_STMT* srev = client_.stmtExec(
      "UPDATE friend_requests SET status = 1 WHERE from_user = ? AND to_user = ? AND status = 0",
      {std::to_string(to), std::to_string(from)}, &errNo);
  if (srev) mysql_stmt_close(srev);
  // 建立联系人关系（双向）
  MYSQL_STMT* s2 = client_.stmtExec(
      "INSERT IGNORE INTO contacts (user_id, contact_id) VALUES (?, ?), (?, ?)",
      {std::to_string(to), std::to_string(from), std::to_string(from), std::to_string(to)},
      &errNo);
  if (!s2) {
    LOG_E("[mysql] 接受请求后建联系人失败 errno=" << errNo);
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
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE friend_requests SET status = 2 WHERE from_user = ? AND to_user = ? AND status = 0",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 拒绝好友请求失败 errno=" << errNo);
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
  if (!client_.query(sql.c_str())) {
    LOG_E("[mysql] 查询好友请求失败: " << client_.errStr());
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

bool ContactRepository::requestTime(int64_t from, int64_t to, int64_t* ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || ts == nullptr) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT UNIX_TIMESTAMP(created_at) FROM friend_requests "
      "WHERE from_user = ? AND to_user = ? LIMIT 1",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 查询好友请求时间失败 errno=" << errNo);
    return false;
  }
  int64_t v = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &v;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 请求时间结果绑定失败: " << client_.errStr());
    mysql_stmt_close(stmt);
    return false;
  }
  const int rc = mysql_stmt_fetch(stmt);
  mysql_stmt_close(stmt);
  if (rc != 0) return false;
  *ts = v;
  return true;
}

} // namespace im
