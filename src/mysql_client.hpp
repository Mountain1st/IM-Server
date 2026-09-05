#pragma once

#include <mysql/mysql.h>

#include <string>
#include <vector>

namespace im {

// MySQL C API 的 RAII 封装：负责连接生命周期与错误信息。
// 注意：单个 MYSQL* 连接非线程安全，调用方须串行访问
// （本项目由 UserStore 的互斥锁保证）。
class MySqlClient {
public:
  MySqlClient(const std::string& host, unsigned port, const std::string& user,
              const std::string& password, const std::string& db);
  ~MySqlClient();

  MySqlClient(const MySqlClient&) = delete;
  MySqlClient& operator=(const MySqlClient&) = delete;

  bool ok() const { return ok_; }
  unsigned errNo() const;
  std::string errStr() const;
  MYSQL* handle() { return conn_; }

private:
  MYSQL* conn_ = nullptr;
  bool ok_ = false;
};

// 用准备好的语句执行参数化 SQL（INSERT/SELECT/UPDATE 通用）。
// 成功返回 stmt（由调用方读取结果后关闭），失败返回 nullptr 并写出 errno。
// 调用方须已持有连接访问锁（单连接非线程安全）。
MYSQL_STMT* mysqlStmtExec(MYSQL* c, const std::string& sql,
                          const std::vector<std::string>& params, int* errNo);

} // namespace im