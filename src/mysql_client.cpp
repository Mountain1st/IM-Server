// ============================================================
// MySqlClient：MySQL C API 连接封装（单连接 + 断线自愈）
// - 构造即连接（失败 ok()=false，由上层决定启动报错退出）
// - 参数化 SQL 经 stmtExec 预处理执行（天然防 SQL 注入）
// - 读写超时 5 秒（MYSQL_OPT_READ/WRITE_TIMEOUT）：MySQL 停机时
//   调用方最多阻塞 5 秒返回失败，不再挂死线程
// - 断线自愈：stmtExec/query 失败且 errno 为 2006/2013/4031
//   （服务端踢/网络断）时重建连接重试一次。生产案例：闲置 19 小时
//   被 wait_timeout 踢掉（4031），登录全挂直到重启——自愈后单次
//   查询透明恢复。连接参数备份于成员，重连在原 handle 上进行
//   （指针不变，调用方 handle() 不失效）
// ============================================================
#include "mysql_client.hpp"

#include "log.hpp"

#include <cstring>
#include <iostream>

namespace im {

namespace {
// 内部执行（不自愈）：参数化 SQL 预处理执行。成功返回 stmt
// （由调用方读取结果后关闭），失败返回 nullptr 并写出 errno。
// 调用方须已持有连接访问锁（单连接非线程安全）
MYSQL_STMT* mysqlStmtExec(MYSQL* c, const std::string& sql,
                          const std::vector<std::string>& params, int* errNo) {
  MYSQL_STMT* stmt = mysql_stmt_init(c);
  if (!stmt) {
    *errNo = static_cast<int>(mysql_errno(c));
    return nullptr;
  }
  if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size())) {
    *errNo = static_cast<int>(mysql_stmt_errno(stmt));
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
    *errNo = static_cast<int>(mysql_stmt_errno(stmt));
    mysql_stmt_close(stmt);
    return nullptr;
  }
  if (mysql_stmt_execute(stmt)) {
    *errNo = static_cast<int>(mysql_stmt_errno(stmt));
    mysql_stmt_close(stmt);
    return nullptr;
  }
  return stmt;
}
} // namespace

MySqlClient::MySqlClient(const std::string& host, unsigned port, const std::string& user,
                         const std::string& password, const std::string& db)
    : host_(host), port_(port), user_(user), password_(password), db_(db) {
  conn_ = mysql_init(nullptr);
  if (!conn_) {
    LOG_E("[mysql] mysql_init 失败");
    return;
  }
  mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  // 读写/连接超时（秒）：MySQL 停顿（或容器重启期 DNS/端口未就绪）时
  // 调用方最多阻塞数秒返回失败，不再无限等待（如停机时 purge 线程
  // join 挂死；部署机连服务名 mysql，容器重启期 connect 可能长等）
  const unsigned kMySqlIoTimeoutSec = 5;
  mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &kMySqlIoTimeoutSec);
  // 禁用 TLS（内网容器服务名/本地连接无需加密；MySQL 8 密码认证在非 TLS
  // 下自动走 RSA 公钥交换，凭据仍不裸奔）：libmysqlclient 的 TLS 握手
  // 存在库级内存泄漏（LSan 实测 37982 次分配不回收，2026-09-30 ASan
  // 自检三轮），禁用后顺带消除——对明文容器网络可接受，公网须经 Nginx
  // wss 终结（入口已加密）
  const enum mysql_ssl_mode kSslMode = SSL_MODE_DISABLED;
  mysql_options(conn_, MYSQL_OPT_SSL_MODE, &kSslMode);

  mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &kMySqlIoTimeoutSec);
  mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &kMySqlIoTimeoutSec);
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

bool MySqlClient::isConnectionLost(unsigned errNo) {
  // 2006 CR_SERVER_GONE_ERROR：服务端已断开（发包前发现）
  // 2013 CR_SERVER_LOST_ERROR：查询过程中丢失连接
  // 4031 ER_CLIENT_INTERACT_TIMEOUT：MySQL 8.x 服务端因不活跃踢出
  //   （wait_timeout 到期；生产 2026-09-29 实际报的就是它）
  return errNo == 2006 || errNo == 2013 || errNo == 4031;
}

bool MySqlClient::reconnect() {
  // 在已断开的 handle 上重新连接：MYSQL* 指针不变（调用方 handle() 与
  // 进行中的锁语义都保持）。MYSQL_OPT_* 选项绑定在 handle 上、重连后
  // 仍生效，但保险起见重设（超时/字符集）
  mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  const unsigned kMySqlIoTimeoutSec = 5;
  mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &kMySqlIoTimeoutSec);
  mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &kMySqlIoTimeoutSec);
  mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &kMySqlIoTimeoutSec);
  const enum mysql_ssl_mode kSslMode = SSL_MODE_DISABLED;
  mysql_options(conn_, MYSQL_OPT_SSL_MODE, &kSslMode);
  if (!mysql_real_connect(conn_, host_.c_str(), user_.c_str(), password_.c_str(),
                          db_.c_str(), port_, nullptr, 0)) {
    // 注意：此处不置 ok_=false——各仓库方法开头有 `if (!client_.ok())`
    // 短路守卫，一旦置 false，MySQL 恢复后也不会再进 stmtExec/query
    // 触发重连，服务将瘫痪到重启（自愈反而退化为不可恢复）。
    // ok_ 只表达"构造时连接成功"（启动 fail-fast 语义）；本次失败
    // 由调用方按查询失败处理（errStr() 实时读 mysql_error）
    LOG_E("[mysql] 自愈重连失败: " << mysql_error(conn_));
    return false;
  }
  LOG_I("[mysql] 连接已自愈重建");
  return true;
}

MYSQL_STMT* MySqlClient::stmtExec(const std::string& sql,
                                  const std::vector<std::string>& params, int* errNo) {
  MYSQL_STMT* stmt = mysqlStmtExec(conn_, sql, params, errNo);
  if (stmt) return stmt;
  // 非连接类错误（语法/约束等）：重连无意义，直接失败
  if (!isConnectionLost(static_cast<unsigned>(*errNo))) return nullptr;
  LOG_W("[mysql] 连接已断（errno=" << *errNo << "），自愈重连后重试");
  if (!reconnect()) return nullptr;
  // 只重试一次：重连成功但再次失败按真实错误返回（防重连风暴）
  return mysqlStmtExec(conn_, sql, params, errNo);
}

bool MySqlClient::query(const std::string& sql) {
  if (mysql_query(conn_, sql.c_str()) == 0) return true;
  const unsigned e = mysql_errno(conn_);
  if (!isConnectionLost(e)) return false;
  LOG_W("[mysql] 连接已断（errno=" << e << "），自愈重连后重试");
  if (!reconnect()) return false;
  return mysql_query(conn_, sql.c_str()) == 0;
}

} // namespace im
