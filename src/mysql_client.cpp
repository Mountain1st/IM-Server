#include "mysql_client.hpp"

#include <iostream>

namespace im {

MySqlClient::MySqlClient(const std::string& host, unsigned port, const std::string& user,
                         const std::string& password, const std::string& db) {
  conn_ = mysql_init(nullptr);
  if (!conn_) {
    std::cerr << "[mysql] mysql_init 失败" << std::endl;
    return;
  }
  mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  if (!mysql_real_connect(conn_, host.c_str(), user.c_str(), password.c_str(),
                          db.c_str(), port, nullptr, 0)) {
    std::cerr << "[mysql] 连接失败: " << mysql_error(conn_) << std::endl;
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

} // namespace im