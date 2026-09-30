// ============================================================
// BlockRepository：拉黑（黑名单）MySQL 存储（v2.4）
// - 单向行存储 (user_id, blocked_id)，双向断联靠 isBlockedPair
//   查 "a|b" 与 "b|a" 两键（两次内存查找，热路径零 DB）
// - 写路径（add/remove）同步刷缓存；绕库直改由 syncCaches
//   计数比对自愈（30 分钟，与其他三缓存同构）
// - 参数化 SQL（防注入）+ 断线自愈（client_.stmtExec）
// ============================================================
#include "block_repository.hpp"

#include "log.hpp"

#include <cstdlib>
#include <cstring>

namespace im {

namespace {
// INSERT 撞唯一键（重复拉黑）的 MySQL 错误码
constexpr unsigned kErrDupEntry = 1062;
} // namespace

BlockRepository::BlockRepository(const std::string& host, unsigned port,
                                 const std::string& user, const std::string& password,
                                 const std::string& db)
    : client_(host, port, user, password, db) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fetchAllBlocks(&blocks_)) return;
    LOG_I("[mysql] 已加载 " << blocks_.size() << " 条拉黑关系到缓存");
  }
}

bool BlockRepository::fetchAllBlocks(std::unordered_set<std::string>* out) {
  // 调用方须持有 mutex_（构造加载与重建共用同一查询）
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  if (!client_.query("SELECT user_id, blocked_id FROM blocks")) {
    LOG_E("[mysql] 查询拉黑关系失败: " << client_.errStr());
    return false;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return false;
  std::unordered_set<std::string> fresh;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0] && row[1]) {
      fresh.emplace(std::string(row[0]) + "|" + std::string(row[1]));
    }
  }
  mysql_free_result(res);
  out->swap(fresh);
  return true;
}

bool BlockRepository::isBlockedPair(int64_t a, int64_t b) {
  std::lock_guard<std::mutex> lock(mutex_);
  return blocks_.count(std::to_string(a) + "|" + std::to_string(b)) != 0 ||
         blocks_.count(std::to_string(b) + "|" + std::to_string(a)) != 0;
}

bool BlockRepository::isOneWayBlockedBy(int64_t a, int64_t b) {
  // a 拉黑了 b？
  std::lock_guard<std::mutex> lock(mutex_);
  return blocks_.count(std::to_string(a) + "|" + std::to_string(b)) != 0;
}

int BlockRepository::addBlock(int64_t userId, int64_t blockedId, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "INSERT INTO blocks (user_id, blocked_id, created_ts) VALUES (?, ?, ?)",
      {std::to_string(userId), std::to_string(blockedId), std::to_string(now)},
      &errNo);
  if (!stmt) {
    if (errNo == static_cast<int>(kErrDupEntry)) return 1; // 重复拉黑
    LOG_E("[mysql] 拉黑写入失败 errno=" << errNo);
    return 2;
  }
  mysql_stmt_close(stmt);
  blocks_.emplace(std::to_string(userId) + "|" + std::to_string(blockedId));
  return 0;
}

int BlockRepository::removeBlock(int64_t userId, int64_t blockedId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "DELETE FROM blocks WHERE user_id = ? AND blocked_id = ?",
      {std::to_string(userId), std::to_string(blockedId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 解除拉黑失败 errno=" << errNo);
    return 2;
  }
  // affected 行数=0 表示未拉黑（409）；成功路径缓存同步摘除
  const int64_t affected = static_cast<int64_t>(mysql_stmt_affected_rows(stmt));
  mysql_stmt_close(stmt);
  if (affected == 0) return 1;
  blocks_.erase(std::to_string(userId) + "|" + std::to_string(blockedId));
  return 0;
}

std::vector<std::pair<int64_t, int64_t>> BlockRepository::listBlocked(int64_t userId) {
  std::vector<std::pair<int64_t, int64_t>> out;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return out;
  int errNo = 0;
  // stmtExec 返回时语句已执行，直接绑定结果读取（仿 listContactIds）
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT blocked_id, created_ts FROM blocks WHERE user_id = ? ORDER BY blocked_id",
      {std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 拉黑列表查询失败 errno=" << errNo);
    return out;
  }
  int64_t blockedId = 0;
  int64_t createdTs = 0;
  MYSQL_BIND bind[2];
  memset(bind, 0, sizeof bind);
  bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
  bind[0].buffer = &blockedId;
  bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
  bind[1].buffer = &createdTs;
  if (mysql_stmt_bind_result(stmt, bind) || mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 拉黑列表结果绑定失败: " << client_.errStr());
    mysql_stmt_close(stmt);
    return out;
  }
  while (mysql_stmt_fetch(stmt) == 0) {
    out.emplace_back(blockedId, createdTs);
  }
  mysql_stmt_close(stmt);
  return out;
}

bool BlockRepository::dbBlockCount(size_t* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  MYSQL* c = client_.handle();
  if (!client_.query("SELECT COUNT(*) FROM blocks")) {
    LOG_E("[mysql] 统计拉黑行数失败: " << client_.errStr());
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

size_t BlockRepository::cacheBlockCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return blocks_.size();
}

void BlockRepository::reloadBlocksCache() {
  // 先查全量到临时集合，成功才换入：查询失败保持旧缓存，
  // 避免"清空后加载失败"把 isBlockedPair 打成全 false（拉黑失效）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fetchAllBlocks(&blocks_)) return;
    LOG_I("[mysql] 拉黑缓存已重建（" << blocks_.size() << " 条，触发：计数漂移）");
  }
}

} // namespace im
