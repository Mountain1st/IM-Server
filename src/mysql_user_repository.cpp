// ============================================================
// MySqlUserRepository：用户表存取（users）
// - registerUser：注册（账号唯一约束，重复回 409）
// - login：账号定位 + PBKDF2-SHA256 密码校验（迭代 10 万次防爆破）+
//   登录失败限流判定（guard 表，见 user_store 洗表防护）
// - fetchSettings：隐私三开关读取
// - 修改/查询全部参数化 SQL；单连接 + 内部互斥锁串行化
// ============================================================
#include "mysql_user_repository.hpp"

#include "log.hpp"

#include <cstring>
#include <iostream>
#include <vector>

namespace im {

namespace {
constexpr unsigned kErrDupEntry = 1062; // MySQL ER_DUP_ENTRY
} // namespace

MySqlUserRepository::MySqlUserRepository(const std::string& host, unsigned port,
                                         const std::string& user,
                                         const std::string& password, const std::string& db)
  : client_(host, port, user, password, db) {
  loadIdCache();
}

void MySqlUserRepository::loadIdCache() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!fetchAllUsers(&ids_, &idAccounts_)) return;
  LOG_I("[mysql] 已加载 " << ids_.size() << " 个用户 id 到缓存");
}

bool MySqlUserRepository::fetchAllUsers(
    std::unordered_map<int64_t, std::string>* ids,
    std::unordered_map<int64_t, std::string>* idAccounts) {
  // 调用方须持有 mutex_（构造加载与重建共用同一查询）
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  if (!client_.query("SELECT id, account, username FROM users")) {
    LOG_E("[mysql] 查询用户缓存失败: " << client_.errStr());
    return false;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return false;
  std::unordered_map<int64_t, std::string> newIds;
  std::unordered_map<int64_t, std::string> newAccounts;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0]) {
      const int64_t id = static_cast<int64_t>(std::strtoll(row[0], nullptr, 10));
      newIds.emplace(id, row[2] ? row[2] : std::string());
      newAccounts.emplace(id, row[1] ? row[1] : std::string());
    }
  }
  mysql_free_result(res);
  ids->swap(newIds);
  idAccounts->swap(newAccounts);
  return true;
}

bool MySqlUserRepository::dbUserCount(size_t* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  if (!client_.query("SELECT COUNT(*) FROM users")) {
    LOG_E("[mysql] 统计用户行数失败: " << client_.errStr());
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

size_t MySqlUserRepository::cacheUserCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ids_.size();
}

void MySqlUserRepository::reloadIdCache() {
  // 先查全量到临时结构，成功才换入：查询失败保持旧缓存，
  // 避免"清空后加载失败"把 userExists 打成全 false（消息全 404）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fetchAllUsers(&ids_, &idAccounts_)) return;
    LOG_I("[mysql] 用户缓存已重建（" << ids_.size() << " 个，触发：计数漂移）");
  }
}

int MySqlUserRepository::insertUser(const std::string& account, const std::string& username,
                                    const std::string& salt, const std::string& passwordHash,
                                    int64_t* userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 500;
  int errNo = 0;
  const std::string sql =
      "INSERT INTO users (account, username, salt, password_hash) VALUES (?, ?, ?, ?)";
  MYSQL_STMT* stmt = client_.stmtExec(sql,
                              {account, username, salt, passwordHash}, &errNo);
  if (!stmt) {
    if (errNo == static_cast<int>(kErrDupEntry)) return 409;
    LOG_E("[mysql] INSERT 失败 errno=" << errNo);
    return 500;
  }
  *userId = static_cast<int64_t>(mysql_stmt_insert_id(stmt));
  mysql_stmt_close(stmt);
  ids_.emplace(*userId, username);
  idAccounts_.emplace(*userId, account);
  return 0;
}

int MySqlUserRepository::fetchUser(const std::string& account, int64_t* userId,
                                   std::string* username, std::string* salt,
                                   std::string* passwordHash) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  const std::string sql =
      "SELECT id, username, salt, password_hash FROM users WHERE account = ? LIMIT 1";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql, {account}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] SELECT 准备/执行失败 errno=" << errNo);
    return 2; // 数据库错误
  }

  int64_t id = 0;
  char nameBuf[128] = {0};
  char saltBuf[64] = {0};
  char hashBuf[96] = {0};
  unsigned long nameLen = 0, saltLen = 0, hashLen = 0;
  bool nameNull = false, saltNull = false, hashNull = false;
  MYSQL_BIND res[4];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &id;
  res[1].buffer_type = MYSQL_TYPE_STRING;
  res[1].buffer = nameBuf;
  res[1].buffer_length = sizeof nameBuf;
  res[1].length = &nameLen;
  res[1].is_null = &nameNull;
  res[2].buffer_type = MYSQL_TYPE_STRING;
  res[2].buffer = saltBuf;
  res[2].buffer_length = sizeof saltBuf;
  res[2].length = &saltLen;
  res[2].is_null = &saltNull;
  res[3].buffer_type = MYSQL_TYPE_STRING;
  res[3].buffer = hashBuf;
  res[3].buffer_length = sizeof hashBuf;
  res[3].length = &hashLen;
  res[3].is_null = &hashNull;

  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] SELECT 结果绑定失败: " << client_.errStr());
    mysql_stmt_close(stmt);
    return 2; // 数据库错误
  }
  int rc = mysql_stmt_fetch(stmt);
  if (rc == 0) {
    *userId = id;
    *username = nameNull ? std::string() : std::string(nameBuf, nameLen);
    *salt = saltNull ? std::string() : std::string(saltBuf, saltLen);
    *passwordHash = hashNull ? std::string() : std::string(hashBuf, hashLen);
    mysql_stmt_close(stmt);
    return 0; // 找到
  }
  mysql_stmt_close(stmt);
  return 1; // 账号不存在
}

int MySqlUserRepository::fetchUserInfo(const std::string& account, int64_t* userId,
                                       std::string* username) {
  // 纯委托 fetchUser：不加锁（fetchUser 的 lock_guard 覆盖整个执行；
  // 若此处再加锁会形成同线程对同一 std::mutex 递归加锁的自死锁——
  // 2026-09-30 自检三轮实测引入过，gdb 抓栈定位）
  std::string salt, passwordHash;
  return fetchUser(account, userId, username, &salt, &passwordHash);
}

bool MySqlUserRepository::userExists(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return ids_.count(userId) != 0;
}

bool MySqlUserRepository::fetchUserNameById(int64_t userId, std::string* username) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = ids_.find(userId);
  if (it == ids_.end()) return false;
  *username = it->second;
  return true;
}

bool MySqlUserRepository::fetchAccountById(int64_t userId, std::string* account) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = idAccounts_.find(userId);
  if (it == idAccounts_.end()) return false;
  *account = it->second;
  return true;
}

int MySqlUserRepository::fetchSettings(int64_t userId, int* allowTempChat, int* searchable,
                                       int* allowAdd) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  const std::string sql = "SELECT allow_temp_chat, searchable, allow_add FROM users WHERE id = ? LIMIT 1";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql, {std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 查询设置失败 errno=" << errNo);
    return 2;
  }
  int v0 = 0, v1 = 0, v2 = 0;
  MYSQL_BIND res[3];
  memset(res, 0, sizeof res);
  for (int i = 0; i < 3; ++i) {
    res[i].buffer_type = MYSQL_TYPE_LONG;
    res[i].buffer = (i == 0) ? &v0 : (i == 1) ? &v1 : &v2;
  }
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 设置结果绑定失败: " << client_.errStr());
    mysql_stmt_close(stmt);
    return 2;
  }
  const int rc = mysql_stmt_fetch(stmt);
  mysql_stmt_close(stmt);
  if (rc == MYSQL_NO_DATA) return 1;  // 用户不存在
  if (rc != 0) return 2;              // 数据库错误
  if (allowTempChat) *allowTempChat = v0;
  if (searchable) *searchable = v1;
  if (allowAdd) *allowAdd = v2;
  return 0;
}

int MySqlUserRepository::updateSettings(int64_t userId, int allowTempChat, int searchable,
                                        int allowAdd) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  // 任一字段 < 0 表示不修改该字段（用 CASE WHEN 保底当前值）
  const std::string sql =
      "UPDATE users SET allow_temp_chat = CASE WHEN ? < 0 THEN allow_temp_chat ELSE ? END, "
      "searchable = CASE WHEN ? < 0 THEN searchable ELSE ? END, "
      "allow_add = CASE WHEN ? < 0 THEN allow_add ELSE ? END WHERE id = ?";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql,
      {std::to_string(allowTempChat), std::to_string(allowTempChat),
       std::to_string(searchable), std::to_string(searchable),
       std::to_string(allowAdd), std::to_string(allowAdd),
       std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 更新设置失败 errno=" << errNo);
    return 2;
  }
  mysql_stmt_close(stmt);
  return 0;
}

void MySqlUserRepository::searchUsers(const std::string& keyword, int64_t excludeUserId,
                                      int limit,
                                      std::vector<std::pair<std::string, std::string>>* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || !out) return;
  if (limit <= 0) limit = 20;
  if (limit > 50) limit = 50;
  std::string kw = keyword;
  if (kw.size() > 64) kw.resize(64);  // 复用 kLoginAccountMaxLen 上限
  std::string esc;
  esc.reserve(kw.size() + 8);
  for (char c : kw) {
    if (c == '%' || c == '_' || c == '\\') esc.push_back('\\');
    esc.push_back(c);
  }
  const std::string like = "%" + esc + "%";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT account, username FROM users WHERE searchable = 1 AND id != ? "
      "AND (account LIKE ? ESCAPE '\\\\' OR username LIKE ? ESCAPE '\\\\') ORDER BY id ASC LIMIT ?",
      {std::to_string(excludeUserId), like, like, std::to_string(limit)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 搜索用户失败 errno=" << errNo);
    return;
  }
  char acctBuf[256] = {0}, nameBuf[256] = {0};
  unsigned long acctLen = 0, nameLen = 0;
  bool acctNull = false, nameNull = false;
  MYSQL_BIND res[2];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_STRING;
  res[0].buffer = acctBuf;
  res[0].buffer_length = sizeof acctBuf;
  res[0].length = &acctLen;
  res[0].is_null = &acctNull;
  res[1].buffer_type = MYSQL_TYPE_STRING;
  res[1].buffer = nameBuf;
  res[1].buffer_length = sizeof nameBuf;
  res[1].length = &nameLen;
  res[1].is_null = &nameNull;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 搜索结果绑定失败: " << client_.errStr());
    mysql_stmt_close(stmt);
    return;
  }
  while (mysql_stmt_fetch(stmt) == 0) {
    out->emplace_back(acctNull ? std::string() : std::string(acctBuf, acctLen),
                      nameNull ? std::string() : std::string(nameBuf, nameLen));
  }
  mysql_stmt_close(stmt);
}

} // namespace im