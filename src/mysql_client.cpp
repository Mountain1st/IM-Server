#include "mysql_client.hpp"

#include "log.hpp"

#include <cstring>
#include <iostream>

namespace im {

MySqlClient::MySqlClient(const std::string& host, unsigned port, const std::string& user,
                         const std::string& password, const std::string& db) {
  conn_ = mysql_init(nullptr);
  if (!conn_) {
    LOG_E("[mysql] mysql_init 失败");
    return;
  }
  mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), password.c_str(),
                          db.c_str(), port, nullptr, 0)) {
    LOG_E("[mysql] 连接失败: " << mysql_error(conn_));
    ok_ = false;
    return;
  }
  ok_ = true;
}

MySqlClient::~MySqlClient() {
  if (conn_) mysql_close(conn_);
}

unsigned MySqlClient::errNo() const {
  return conn_ ? mysql_errno(conn_) : 0;
}

std::string MySqlClient::errStr() const {
  return conn_ ? mysql_error(conn_) : "no mysql connection";
}

MYSQL_STMT* mysqlStmtExec(MYSQL* c, const std::string& sql,
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

} // namespace im