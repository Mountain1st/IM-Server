#include "mysql_user_repository.hpp"

#include <cstring>
#include <iostream>
#include <vector>

namespace im {

namespace {
constexpr unsigned kErrDupEntry = 1062; // MySQL ER_DUP_ENTRY

// 用准备好的语句执行参数化 SQL（INSERT/SELECT 通用）。成功返回 stmt（由调用方读取结果后关闭），失败返回 nullptr。
MYSQL_STMT* stmtExec(MYSQL* c, const std::string& sql,
                     const std::vector<std::string>& params, int* errNo) {
  MYSQL_STMT* stmt = mysql_stmt_init(c);
  if (!stmt) {
    *errNo = mysql_errno(c);
    return nullptr;
  }
  if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size())) {
    *errNo = mysql_stmt_errno(stmt);
    mysql_stmt_close(stmt);
    return nullptr;
  }
  std::vector<MYSQL_BIND> binds(params.size());
  std::vector<unsigned long> lens(params.size());
  for (size_t i = 0; i < params.size(); ++i) {
    memset(&binds[i], 0, sizeof binds[i]);
    lens[i] = static_cast<unsigned long>(params[i].size());
    binds[i].buffer_type = MYSQL_TYPE_STRING;
    binds[i].buffer = const_cast<char*>(params[i].data());
    binds[i].buffer_length = lens[i];
    binds[i].length = &lens[i];
  }
  if (!params.empty() && mysql_stmt_bind_param(stmt, binds.data())) {
    *errNo = mysql_stmt_errno(stmt);
    mysql_stmt_close(stmt);
    return nullptr;
  }
  if (mysql_stmt_execute(stmt)) {
    *errNo = mysql_stmt_errno(stmt);
    mysql_stmt_close(stmt);
    return nullptr;
  }
  return stmt;
}
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
  if (mysql_query(c, "SELECT id, username FROM users")) {
    std::cerr << "[mysql] 加载用户 id 缓存失败: " << client_.errStr() << std::endl;
    return;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0]) ids_.emplace(static_cast<int64_t>(std::strtoll(row[0], nullptr, 10)), row[1] ? row[1] : std::string());
  }
  mysql_free_result(res);
  std::cout << "[mysql] 已加载 " << ids_.size() << " 个用户 id 到缓存" << std::endl;
}

int MySqlUserRepository::insertUser(const std::string& username, const std::string& salt,
                                    const std::string& passwordHash, int64_t* userId) {
  if (!client_.ok()) return 500;
  int errNo = 0;
  const std::string sql =
      "INSERT INTO users (username, salt, password_hash) VALUES (?, ?, ?)";
  MYSQL_STMT* stmt = stmtExec(client_.handle(), sql, {username, salt, passwordHash}, &errNo);
  if (!stmt) {
    if (errNo == static_cast<int>(kErrDupEntry)) return 409;
    std::cerr << "[mysql] INSERT 失败 errno=" << errNo << std::endl;
    return 500;
  }
  *userId = static_cast<int64_t>(mysql_stmt_insert_id(stmt));
  mysql_stmt_close(stmt);
  ids_.emplace(*userId, username);
  return 0;
}

bool MySqlUserRepository::fetchUser(const std::string& username, int64_t* userId,
                                    std::string* salt, std::string* passwordHash) {
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  const std::string sql =
      "SELECT id, salt, password_hash FROM users WHERE username = ? LIMIT 1";
  int errNo = 0;
  MYSQL_STMT* stmt = stmtExec(c, sql, {username}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] SELECT 准备/执行失败 errno=" << errNo << std::endl;
    return false;
  }

  int64_t id = 0;
  char saltBuf[64] = {0};
  char hashBuf[96] = {0};
  unsigned long saltLen = 0, hashLen = 0;
  bool saltNull = false, hashNull = false;
  MYSQL_BIND res[3];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &id;
  res[1].buffer_type = MYSQL_TYPE_STRING;
  res[1].buffer = saltBuf;
  res[1].buffer_length = sizeof saltBuf;
  res[1].length = &saltLen;
  res[1].is_null = &saltNull;
  res[2].buffer_type = MYSQL_TYPE_STRING;
  res[2].buffer = hashBuf;
  res[2].buffer_length = sizeof hashBuf;
  res[2].length = &hashLen;
  res[2].is_null = &hashNull;

  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    std::cerr << "[mysql] SELECT 结果绑定失败: " << client_.errStr() << std::endl;
    mysql_stmt_close(stmt);
    return false;
  }
  bool found = false;
  if (mysql_stmt_fetch(stmt) == 0) {
    *userId = id;
    *salt = saltNull ? std::string() : std::string(saltBuf, saltLen);
    *passwordHash = hashNull ? std::string() : std::string(hashBuf, hashLen);
    found = true;
  }
  mysql_stmt_close(stmt);
  return found;
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

} // namespace im