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
  std::lock_guard<std::mutex> lock(mutex_);
  if (!fetchActiveMembers(&members_)) return;
  LOG_I("[mysql] 已加载 " << members_.size() << " 条群成员关系到缓存");
}

bool GroupRepository::fetchActiveMembers(std::unordered_set<std::string>* out) {
  // 调用方须持有 mutex_（构造加载与重建共用同一查询）
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  if (!client_.query("SELECT group_id, user_id FROM group_members WHERE left_ts = 0")) {
    LOG_E("[mysql] 查询群成员失败: " << client_.errStr());
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

bool GroupRepository::dbMemberCount(size_t* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  if (!client_.query("SELECT COUNT(*) FROM group_members WHERE left_ts = 0")) {
    LOG_E("[mysql] 统计群成员行数失败: " << client_.errStr());
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

size_t GroupRepository::cacheMemberCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return members_.size();
}

void GroupRepository::reloadMembersCache() {
  // 先查全量到临时集合，成功才换入：查询失败保持旧缓存，
  // 避免"先清空再加载"在失败时把缓存打成空（isMember 全 false，群发全 403）
  std::unordered_set<std::string> fresh;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fetchActiveMembers(&fresh)) return;
    members_.swap(fresh);
    LOG_I("[mysql] 群成员缓存已重建（" << members_.size() << " 条，触发：计数漂移）");
  }
}

int64_t GroupRepository::createGroup(const std::string& name, int64_t ownerId,
                                      int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
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
  MYSQL_STMT* s2 = client_.stmtExec(
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
  MYSQL_STMT* stmt = client_.stmtExec(
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
  MYSQL_STMT* stmt = client_.stmtExec(
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
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE `groups` SET owner_id = ? WHERE group_id = ? AND dissolved = 0",
      {std::to_string(newOwnerId), std::to_string(groupId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 转让群主失败 errno=" << errNo);
    return false;
  }
  // 要求恰好影响 1 行：并发解散赢了竞态时返回 false，上层回 404 群已解散
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  return affected == 1;
}

bool GroupRepository::dissolveGroup(int64_t groupId, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
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
  std::unique_lock<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec( "UPDATE `groups` SET name = ? WHERE group_id = ? AND dissolved = 0",
      {name, std::to_string(groupId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 群改名失败 errno=" << errNo);
    return false;
  }
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  if (affected == 1) return true;
  // 同值更新 affected=0 属正常，回查区分：存在且未解散则仍算成功
  lock.unlock();
  bool dissolved = false;
  if (!getGroup(groupId, nullptr, nullptr, nullptr, nullptr, &dissolved)) return false;
  return !dissolved;
}

bool GroupRepository::setAnnouncement(int64_t groupId, const std::string& text,
                                      int64_t now) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE `groups` SET announcement = ?, announce_ts = ? WHERE group_id = ? "
      "AND dissolved = 0",
      {text, std::to_string(now), std::to_string(groupId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 发布群公告失败 errno=" << errNo);
    return false;
  }
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  if (affected == 1) return true;
  // 同值更新 affected=0 属正常，回查区分：存在且未解散则仍算成功
  lock.unlock();
  bool dissolved = false;
  if (!getGroup(groupId, nullptr, nullptr, nullptr, nullptr, &dissolved)) return false;
  return !dissolved;
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
  MYSQL_STMT* stmt = client_.stmtExec(
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
  MYSQL_STMT* stmt = client_.stmtExec(
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

int GroupRepository::addMemberCapped(int64_t groupId, int64_t userId, int64_t now,
                                     size_t cap) {
  // 上限校验与插入在同一临界区内原子完成（B4 自检三轮）：此前 invite 的
  // activeCount 与 addMember 是两个锁区间，群主多端并发拉人可双双读到
  // 49 而都插入成功，把上限击穿到 cap+1（TOCTOU）
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2; // DB 错误
  // 现成员数以缓存计数（members_ 与库由本类所有写路径同步维护；
  // syncCaches 比对兜底直改库漂移）
  size_t n = 0;
  const std::string prefix = std::to_string(groupId) + "|";
  for (const auto& k : members_) {
    if (k.compare(0, prefix.size(), prefix) == 0) ++n;
  }
  if (n >= cap) return 1; // 已满
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "INSERT INTO group_members (group_id, user_id, joined_ts) VALUES (?, ?, ?) "
      "ON DUPLICATE KEY UPDATE joined_ts = VALUES(joined_ts), left_ts = 0",
      {std::to_string(groupId), std::to_string(userId), std::to_string(now)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 群加人失败 errno=" << errNo);
    return 2;
  }
  mysql_stmt_close(stmt);
  members_.emplace(prefix + std::to_string(userId));
  return 0; // 成功
}

bool GroupRepository::removeMember(int64_t groupId, int64_t userId, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
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
  if (!client_.query(sql.c_str())) return 0;
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
  MYSQL_STMT* stmt = client_.stmtExec(
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
  MYSQL_STMT* stmt = client_.stmtExec(
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

void GroupRepository::getDeliveredCursor(int64_t groupId, int64_t userId, int64_t* ts,
                                         int64_t* id) {
  if (ts) *ts = 0;
  if (id) *id = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT last_delivered_ts, last_delivered_id FROM group_members "
      "WHERE group_id = ? AND user_id = ? LIMIT 1",
      {std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) return;
  int64_t vts = 0, vid = 0;
  MYSQL_BIND res[2];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &vts;
  res[1].buffer_type = MYSQL_TYPE_LONGLONG;
  res[1].buffer = &vid;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
      mysql_stmt_fetch(stmt) == 0) {
    if (ts) *ts = vts;
    if (id) *id = vid;
  }
  mysql_stmt_close(stmt);
}

bool GroupRepository::advanceDeliveredCursor(int64_t groupId, int64_t userId, int64_t ts,
                                             int64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  // 严格只进：仅当 (newTs,newId) 字典序 > 旧游标才更新。
  // SQL 内比较，单语句原子，免读改写 TOCTOU；重复/回退 ACK 命中 0 行=幂等成功。
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE group_members SET last_delivered_ts = ?, last_delivered_id = ? "
      "WHERE group_id = ? AND user_id = ? AND "
      "((last_delivered_ts < ?) OR (last_delivered_ts = ? AND last_delivered_id < ?))",
      {std::to_string(ts), std::to_string(id), std::to_string(groupId),
       std::to_string(userId), std::to_string(ts), std::to_string(ts),
       std::to_string(id)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 推进群投递游标失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  return true;
}

}  // namespace im
