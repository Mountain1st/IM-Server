#pragma once

#include <mysql/mysql.h>

#include <string>
#include <vector>

namespace im {

// MySQL C API 的 RAII 封装：负责连接生命周期与错误信息。
// 注意：单个 MYSQL* 连接非线程安全，调用方须串行访问
// （本项目由各 Repository 的互斥锁保证——自愈重连也发生在锁内）。
//
// 断线自愈（2026-09-29，生产故障根治）：
// MySQL 服务端会因空闲超时踢掉连接（wait_timeout，服务端错误 4031），
// 或网络中断（客户端错误 2006/2013）。单条长连接被踢后不自愈会导致
// 之后所有查询持续失败直到重启进程。stmtExec/query 两个入口在
// 失败且 errno 属于"连接已断"类时自动重建连接并重试一次。
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

  // ---- 自愈执行入口（调用方须已持有连接访问锁）----
  // 参数化 SQL 执行：连接断开类错误时重建连接重试一次。
  // 成功返回 stmt（由调用方读取结果后关闭），失败返回 nullptr 并写出 errno
  MYSQL_STMT* stmtExec(const std::string& sql,
                       const std::vector<std::string>& params, int* errNo);
  // 普通语句（无参数）：同自愈语义。成功返回 true（结果集由调用方经
  // handle() 的 mysql_store_result 读取——重建后 handle 指针不变）
  bool query(const std::string& sql);

private:
  // errno 是否属于"连接已断"（可重建自愈）：2006 服务端已断开 /
  // 2013 查询中丢失连接 / 4031 服务端不活跃超时踢出（MySQL 8.x）
  static bool isConnectionLost(unsigned errNo);
  // 在断开的 handle 上重建连接（MYSQL* 指针保持不变，调用方持有的
  // handle() 仍有效）。失败返回 false（不关 ok_，见实现内注释）
  bool reconnect();

  MYSQL* conn_ = nullptr;
  bool ok_ = false;
  // 连接参数备份（自愈重连用）
  std::string host_;
  unsigned port_ = 0;
  std::string user_;
  std::string password_;
  std::string db_;
};

} // namespace im