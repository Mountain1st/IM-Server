#include "group_repository.hpp"

#include "log.hpp"

#include <cstdlib>
#include <cstring>

namespace im {

GroupRepository::GroupRepository(const std::string& host, unsigned port,
                                 const std::string& user, const std::string& password,
                                 const std::string& db)
    : client_(host, port, user, password, db) {
  loadMembersCache();
}

void GroupRepository::loadMembersCache() {
  if (!client_.ok()) return;
  MYSQL* c = client_.handle();
  if (mysql_query(c, "SELECT group_id, user_id FROM group_members WHERE left_ts = 0")) {
    LOG_E("[mysql] 加载群成员缓存失败: " << client_.errStr());
    return;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0] && row[1]) {
      members_.emplace(std::string(row[0]) + "|" + row[1]);
    }
  }
  mysql_free_result(res);
  LOG_I("[mysql] 已加载 " << members_.size() << " 条群成员关系到缓存");
}

int64_t GroupRepository::createGroup(const std::string& name, int64_t ownerId,
                                      int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "INSERT INTO `groups` (name, owner_id, created_ts, announcement, announce_ts, "
      "dissolved, dissolved_ts) VALUES (?, ?, ?, '', 0, 0, 0)",
      {name, std::to_string(ownerId), std::to_string(now)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 建群失败 errno=" << errNo);
    return 0;
  }
  const int64_t gid = static_cast<int64_t>(mysql_stmt_insert_id(stmt));
  mysql_stmt_close(stmt);
  if (gid <= 0) return 0;
  // 建群者自动入群
  MYSQL_STMT* s2 = mysqlStmtExec(
      client_.handle(),
      "INSERT INTO group_members (group_id, user_id, joined_ts) VALUES (?, ?, ?)",
      {std::to_string(gid), std::to_string(ownerId), std::to_string(now)}, &errNo);
  if (!s2) {
    LOG_E("[mysql] 建群者入群失败 errno=" << errNo);
    return 0;
  }
  mysql_stmt_close(s2);
  members_.emplace(std::to_string(gid) + "|" + std::to_string(ownerId));
  return gid;
}

bool GroupRepository::groupExists(int64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || groupId <= 0) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
                                   "SELECT 1 FROM `groups` WHERE group_id = ? LIMIT 1",
                                   {std::to_string(groupId)}, &errNo);
  if (!stmt) return false;
  int one = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONG;
  res[0].buffer = &one;
  bool found = false;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt)) {
    found = (mysql_stmt_fetch(stmt) == 0);
  }
  mysql_stmt_close(stmt);
  return found;
}

bool GroupRepository::getGroup(int64_t groupId, std::string* name, int64_t* ownerId,
                                std::string* announcement, int64_t* announceTs,
                                bool* dissolved) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || groupId <= 0) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT name, owner_id, announcement, announce_ts, dissolved FROM `groups` "
      "WHERE group_id = ? LIMIT 1",
      {std::to_string(groupId)}, &errNo);
  if (!stmt) return false;
  char nameBuf[128] = {0};
  unsigned long nameLen = 0;
  bool nameNull = false;
  int64_t oid = 0;
  // 公告上限 1024 字节，2048 字节栈缓冲足够
  char annBuf[2048] = {0};
  unsigned long annLen = 0;
  bool annNull = false;
  int64_t annTs = 0;
  bool annTsNull = false;
  signed char dissolvedTiny = 0;
  bool dissolvedNull = false;
  MYSQL_BIND res[5];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_STRING;
  res[0].buffer = nameBuf;
  res[0].buffer_length = sizeof nameBuf;
  res[0].length = &nameLen;
  res[0].is_null = &nameNull;
  res[1].buffer_type = MYSQL_TYPE_LONGLONG;
  res[1].buffer = &oid;
  res[2].buffer_type = MYSQL_TYPE_STRING;
  res[2].buffer = annBuf;
  res[2].buffer_length = sizeof annBuf;
  res[2].length = &annLen;
  res[2].is_null = &annNull;
  res[3].buffer_type = MYSQL_TYPE_LONGLONG;
  res[3].buffer = &annTs;
  res[3].is_null = &annTsNull;
  res[4].buffer_type = MYSQL_TYPE_TINY;
  res[4].buffer = &dissolvedTiny;
  res[4].is_null = &dissolvedNull;
  bool found = false;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
      mysql_stmt_fetch(stmt) == 0) {
    if (name) *name = nameNull ? std::string() : std::string(nameBuf, nameLen);
    if (ownerId) *ownerId = oid;
    if (announcement)
      *announcement = annNull ? std::string() : std::string(annBuf, annLen);
    if (announceTs) *announceTs = annTsNull ? 0 : annTs;
    if (dissolved) *dissolved = !dissolvedNull && dissolvedTiny != 0;
    found = true;
  }
  mysql_stmt_close(stmt);
  return found;
}

bool GroupRepository::setOwner(int64_t groupId, int64_t newOwnerId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(), "UPDATE `groups` SET owner_id = ? WHERE group_id = ?",
      {std::to_string(newOwnerId), std::to_string(groupId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 转让群主失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  return true;
}

bool GroupRepository::dissolveGroup(int64_t groupId, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "UPDATE `groups` SET dissolved = 1, dissolved_ts = ? WHERE group_id = ? "
      "AND dissolved = 0",
      {std::to_string(now), std::to_string(groupId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 解散群失败 errno=" << errNo);
    return false;
  }
  // 要求恰好影响 1 行：重复解散返回 false，上层回 404 群已解散
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  return affected == 1;
}

bool GroupRepository::renameGroup(int64_t groupId, const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(), "UPDATE `groups` SET name = ? WHERE group_id = ? AND dissolved = 0",
      {name, std::to_string(groupId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 群改名失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  return true;
}

bool GroupRepository::setAnnouncement(int64_t groupId, const std::string& text,
                                      int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "UPDATE `groups` SET announcement = ?, announce_ts = ? WHERE group_id = ? "
      "AND dissolved = 0",
      {text, std::to_string(now), std::to_string(groupId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 发布群公告失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  return true;
}

bool GroupRepository::isMember(int64_t groupId, int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return members_.count(std::to_string(groupId) + "|" + std::to_string(userId)) != 0;
}

bool GroupRepository::memberPeriod(int64_t groupId, int64_t userId, int64_t* joinedTs,
                                   int64_t* leftTs) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT joined_ts, left_ts FROM group_members WHERE group_id = ? AND user_id = ? LIMIT 1",
      {std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) return false;
  int64_t j = 0, l = 0;
  MYSQL_BIND res[2];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &j;
  res[1].buffer_type = MYSQL_TYPE_LONGLONG;
  res[1].buffer = &l;
  bool found = false;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
      mysql_stmt_fetch(stmt) == 0) {
    if (joinedTs) *joinedTs = j;
    if (leftTs) *leftTs = l;
    found = true;
  }
  mysql_stmt_close(stmt);
  return found;
}

bool GroupRepository::addMember(int64_t groupId, int64_t userId, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  // 新加或重入：重入时 joined_ts=now、left_ts=0（旧时段自然不可见）
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "INSERT INTO group_members (group_id, user_id, joined_ts) VALUES (?, ?, ?) "
      "ON DUPLICATE KEY UPDATE joined_ts = VALUES(joined_ts), left_ts = 0",
      {std::to_string(groupId), std::to_string(userId), std::to_string(now)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 群加人失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  members_.emplace(std::to_string(groupId) + "|" + std::to_string(userId));
  return true;
}

bool GroupRepository::removeMember(int64_t groupId, int64_t userId, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "UPDATE group_members SET left_ts = ? WHERE group_id = ? AND user_id = ? "
      "AND left_ts = 0",
      {std::to_string(now), std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 群退群失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  members_.erase(std::to_string(groupId) + "|" + std::to_string(userId));
  return true;
}

size_t GroupRepository::activeCount(int64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  const std::string sql = "SELECT COUNT(*) FROM group_members WHERE group_id = " +
                          std::to_string(groupId) + " AND left_ts = 0";
  if (mysql_query(client_.handle(), sql.c_str())) return 0;
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return 0;
  size_t n = 0;
  if (MYSQL_ROW row = mysql_fetch_row(res)) {
    if (row[0]) n = static_cast<size_t>(std::strtoull(row[0], nullptr, 10));
  }
  mysql_free_result(res);
  return n;
}

std::vector<int64_t> GroupRepository::listMemberIds(int64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int64_t> out;
  if (!client_.ok()) return out;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT user_id FROM group_members WHERE group_id = ? AND left_ts = 0",
      {std::to_string(groupId)}, &errNo);
  if (!stmt) return out;
  int64_t uid = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &uid;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt)) {
    while (mysql_stmt_fetch(stmt) == 0) out.push_back(uid);
  }
  mysql_stmt_close(stmt);
  return out;
}

std::vector<int64_t> GroupRepository::listUserGroups(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int64_t> out;
  if (!client_.ok()) return out;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT group_id FROM group_members WHERE user_id = ? AND left_ts = 0",
      {std::to_string(userId)}, &errNo);
  if (!stmt) return out;
  int64_t gid = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &gid;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt)) {
    while (mysql_stmt_fetch(stmt) == 0) out.push_back(gid);
  }
  mysql_stmt_close(stmt);
  return out;
}

int64_t GroupRepository::getWatermark(int64_t groupId, int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT last_delivered_ts FROM group_members WHERE group_id = ? AND user_id = ? "
      "LIMIT 1",
      {std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) return 0;
  int64_t v = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &v;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt) ||
      mysql_stmt_fetch(stmt) != 0) {
    v = 0;
  }
  mysql_stmt_close(stmt);
  return v;
}

void GroupRepository::setWatermark(int64_t groupId, int64_t userId, int64_t ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "UPDATE group_members SET last_delivered_ts = ? WHERE group_id = ? AND user_id = ?",
      {std::to_string(ts), std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 更新群水位线失败 errno=" << errNo);
    return;
  }
  mysql_stmt_close(stmt);
}

}  // namespace im
