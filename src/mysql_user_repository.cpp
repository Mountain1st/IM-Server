#include "mysql_user_repository.hpp"

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
  if (!client_.ok()) return;
  MYSQL* c = client_.handle();
  if (mysql_query(c, "SELECT id, account, username FROM users")) {
    std::cerr << "[mysql] 加载用户 id 缓存失败: " << client_.errStr() << std::endl;
    return;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0]) {
      const int64_t id = static_cast<int64_t>(std::strtoll(row[0], nullptr, 10));
      ids_.emplace(id, row[2] ? row[2] : std::string());
      idAccounts_.emplace(id, row[1] ? row[1] : std::string());
    }
  }
  mysql_free_result(res);
  std::cout << "[mysql] 已加载 " << ids_.size() << " 个用户 id 到缓存" << std::endl;
}

int MySqlUserRepository::insertUser(const std::string& account, const std::string& username,
                                    const std::string& salt, const std::string& passwordHash,
                                    int64_t* userId) {
  if (!client_.ok()) return 500;
  int errNo = 0;
  const std::string sql =
      "INSERT INTO users (account, username, salt, password_hash) VALUES (?, ?, ?, ?)";
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(), sql,
                              {account, username, salt, passwordHash}, &errNo);
  if (!stmt) {
    if (errNo == static_cast<int>(kErrDupEntry)) return 409;
    std::cerr << "[mysql] INSERT 失败 errno=" << errNo << std::endl;
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
  if (!client_.ok()) return 2;
  MYSQL* c = client_.handle();
  const std::string sql =
      "SELECT id, username, salt, password_hash FROM users WHERE account = ? LIMIT 1";
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(c, sql, {account}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] SELECT 准备/执行失败 errno=" << errNo << std::endl;
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
    std::cerr << "[mysql] SELECT 结果绑定失败: " << client_.errStr() << std::endl;
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
  std::string salt, passwordHash;
  return fetchUser(account, userId, username, &salt, &passwordHash);
}

bool MySqlUserRepository::userExists(int64_t userId) {
  return ids_.count(userId) != 0;
}

bool MySqlUserRepository::fetchUserNameById(int64_t userId, std::string* username) {
  auto it = ids_.find(userId);
  if (it == ids_.end()) return false;
  *username = it->second;
  return true;
}

bool MySqlUserRepository::fetchAccountById(int64_t userId, std::string* account) {
  auto it = idAccounts_.find(userId);
  if (it == idAccounts_.end()) return false;
  *account = it->second;
  return true;
}

} // namespace im