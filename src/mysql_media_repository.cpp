// ============================================================
// MySqlMediaRepository：媒体表（media）存取
// - insertPending：预签名后落一条 PENDING 记录
// - findByFileId：按 file_id 查单条（file_id 唯一）
// - commit：PENDING→COMMITTED，写入以 HEAD 为准的 size/元数据；
//   已 COMMITTED 视为成功（幂等），状态查询在锁内完成避免自死锁
// - markDeleted：标记 status=GC（按 file_id 精确更新）
// - findPendingOlderThan：供 GC 回收超期 PENDING（olderThanSec 为绝对时间戳上界）
// - isReferenced：media 是否被任何 messages 行引用（media_id 外键）；
//   返回 1=被引用，0=确定未引用，-1=查询失败（未知）
// MySQL 单连接非线程安全：所有方法持 mutex_ 串行化
// ============================================================
#include "mysql_media_repository.hpp"

#include "log.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace im {

namespace {
// 查询列顺序：id,file_id,uploader_id,object_key,file_name,mime_type,file_size,
//             width,height,duration,thumb_key,status,created_at
constexpr int kMediaCols = 13;
constexpr const char* kMediaSelect =
    "SELECT id,file_id,uploader_id,object_key,file_name,mime_type,file_size,"
    "width,height,duration,thumb_key,status,created_at FROM media WHERE file_id=?";

// 结果集列缓冲（mysql_stmt_fetch 后按 buffer/length/is_null 组装字符串）
struct MediaRowBuf {
  char id[32] = {0};
  char fileId[64] = {0};
  char uploaderId[32] = {0};
  char objectKey[512] = {0};
  char fileName[512] = {0};
  char mimeType[128] = {0};
  char fileSize[32] = {0};
  char width[32] = {0};
  char height[32] = {0};
  char duration[32] = {0};
  char thumbKey[512] = {0};
  char status[16] = {0};
  char createdAt[32] = {0};
  bool isNull[kMediaCols] = {false};
  unsigned long len[kMediaCols] = {0};
};

// 绑定 13 列结果缓冲；列顺序须与 kMediaSelect 一致
void bindMediaResult(MYSQL_STMT* stmt, MediaRowBuf& b) {
  MYSQL_BIND res[kMediaCols];
  memset(res, 0, sizeof res);
  auto set = [&](int i, char* buf, size_t cap) {
    res[i].buffer_type = MYSQL_TYPE_STRING;
    res[i].buffer = buf;
    res[i].buffer_length = static_cast<unsigned long>(cap);
    res[i].length = &b.len[i];
    res[i].is_null = &b.isNull[i];
  };
  set(0, b.id, sizeof b.id);
  set(1, b.fileId, sizeof b.fileId);
  set(2, b.uploaderId, sizeof b.uploaderId);
  set(3, b.objectKey, sizeof b.objectKey);
  set(4, b.fileName, sizeof b.fileName);
  set(5, b.mimeType, sizeof b.mimeType);
  set(6, b.fileSize, sizeof b.fileSize);
  set(7, b.width, sizeof b.width);
  set(8, b.height, sizeof b.height);
  set(9, b.duration, sizeof b.duration);
  set(10, b.thumbKey, sizeof b.thumbKey);
  set(11, b.status, sizeof b.status);
  set(12, b.createdAt, sizeof b.createdAt);
  mysql_stmt_bind_result(stmt, res);
}

std::string colStr(const char* buf, bool isNull, unsigned long len) {
  if (isNull) return std::string();
  return std::string(buf, len);
}

// 由已 fetch 成功的 stmt 组装 MediaRecord
MediaRecord rowToMedia(const MediaRowBuf& b) {
  MediaRecord m;
  m.id = std::strtoll(colStr(b.id, b.isNull[0], b.len[0]).c_str(), nullptr, 10);
  m.fileId = colStr(b.fileId, b.isNull[1], b.len[1]);
  m.uploaderId =
      std::strtoll(colStr(b.uploaderId, b.isNull[2], b.len[2]).c_str(), nullptr, 10);
  m.objectKey = colStr(b.objectKey, b.isNull[3], b.len[3]);
  m.fileName = colStr(b.fileName, b.isNull[4], b.len[4]);
  m.mimeType = colStr(b.mimeType, b.isNull[5], b.len[5]);
  m.fileSize =
      std::strtoll(colStr(b.fileSize, b.isNull[6], b.len[6]).c_str(), nullptr, 10);
  m.width = std::atoi(colStr(b.width, b.isNull[7], b.len[7]).c_str());
  m.height = std::atoi(colStr(b.height, b.isNull[8], b.len[8]).c_str());
  m.duration = std::atoi(colStr(b.duration, b.isNull[9], b.len[9]).c_str());
  m.thumbKey = colStr(b.thumbKey, b.isNull[10], b.len[10]);
  m.status = std::atoi(colStr(b.status, b.isNull[11], b.len[11]).c_str());
  m.createdAt =
      std::strtoll(colStr(b.createdAt, b.isNull[12], b.len[12]).c_str(), nullptr, 10);
  return m;
}

// 无锁查询（调用方须已持有 mutex_）：按 file_id 取单条，未命中返回 nullopt
std::optional<MediaRecord> queryByFileIdLocked(MySqlClient& client,
                                               const std::string& fileId) {
  int errNo = 0;
  MYSQL_STMT* stmt = client.stmtExec(kMediaSelect, {fileId}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 查询媒体失败 errno=" << errNo);
    return std::nullopt;
  }
  MediaRowBuf b;
  bindMediaResult(stmt, b);
  if (mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 媒体结果存储失败: " << client.errStr());
    mysql_stmt_close(stmt);
    return std::nullopt;
  }
  std::optional<MediaRecord> out;
  if (mysql_stmt_fetch(stmt) == 0) out = rowToMedia(b);
  mysql_stmt_close(stmt);
  return out;
}
}  // namespace

MySqlMediaRepository::MySqlMediaRepository(const std::string& host, unsigned port,
                                           const std::string& user,
                                           const std::string& password,
                                           const std::string& db)
    : client_(host, port, user, password, db) {}

bool MySqlMediaRepository::insertPending(const MediaRecord& m) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  const std::string sql =
      "INSERT INTO media (file_id,uploader_id,object_key,file_name,mime_type,"
      "file_size,width,height,duration,thumb_key,status,created_at) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,0,?)";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      sql,
      {m.fileId, std::to_string(m.uploaderId), m.objectKey, m.fileName, m.mimeType,
       std::to_string(m.fileSize), std::to_string(m.width), std::to_string(m.height),
       std::to_string(m.duration), m.thumbKey, std::to_string(m.createdAt)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 插入媒体失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  return true;
}

std::optional<MediaRecord> MySqlMediaRepository::findByFileId(
    const std::string& fileId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return std::nullopt;
  return queryByFileIdLocked(client_, fileId);
}

bool MySqlMediaRepository::commit(const std::string& fileId, int64_t size, int width,
                                  int height, int duration) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  const std::string sql =
      "UPDATE media SET status=2,file_size=?,width=?,height=?,duration=? "
      "WHERE file_id=? AND status=0";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      sql,
      {std::to_string(size), std::to_string(width), std::to_string(height),
       std::to_string(duration), fileId},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 提交媒体失败 errno=" << errNo);
    return false;
  }
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  if (affected > 0) return true;
  // 幂等：本次未更新（已非 PENDING）时，仅当已处于 COMMITTED 视为成功。
  // 用 == 而非 >=：避免把已 GC（status=3）误判为 commit 成功
  auto cur = queryByFileIdLocked(client_, fileId);
  if (cur && cur->status == kMediaCommitted) return true;
  return false;
}

bool MySqlMediaRepository::markDeleted(const std::string& fileId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  // 仅允许 PENDING(0) → GC(3)：防止 GC 与 commit 竞态误将已 COMMITTED(2)
  // 的记录置为已删除（对象已被 GC 删除会造成不可逆数据丢失）。
  const std::string sql = "UPDATE media SET status=3 WHERE file_id=? AND status=0";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql, {fileId}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 标记媒体删除失败 errno=" << errNo);
    return false;
  }
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  return affected > 0;
}

std::vector<MediaRecord> MySqlMediaRepository::findPendingOlderThan(
    int64_t olderThanSec, int limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MediaRecord> out;
  if (!client_.ok()) return out;
  if (limit <= 0) limit = 100;
  if (limit > 1000) limit = 1000;
  const std::string sql =
      "SELECT id,file_id,uploader_id,object_key,file_name,mime_type,file_size,"
      "width,height,duration,thumb_key,status,created_at FROM media "
      "WHERE status=0 AND created_at<? ORDER BY created_at ASC LIMIT " +
      std::to_string(limit);
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql, {std::to_string(olderThanSec)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 查询超期媒体失败 errno=" << errNo);
    return out;
  }
  MediaRowBuf b;
  bindMediaResult(stmt, b);
  if (mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 超期媒体结果存储失败: " << client_.errStr());
    mysql_stmt_close(stmt);
    return out;
  }
  while (mysql_stmt_fetch(stmt) == 0) out.push_back(rowToMedia(b));
  mysql_stmt_close(stmt);
  return out;
}

std::vector<MediaRecord> MySqlMediaRepository::findCommittedOrphanOlderThan(
    int64_t olderThanSec, int limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MediaRecord> out;
  if (!client_.ok()) return out;
  if (limit <= 0) limit = 100;
  if (limit > 1000) limit = 1000;
  // NOT EXISTS 子查询在 SQL 内判定引用，避免「先查列表再逐条查引用」的竞态放大
  const std::string sql =
      "SELECT id,file_id,uploader_id,object_key,file_name,mime_type,file_size,"
      "width,height,duration,thumb_key,status,created_at FROM media m "
      "WHERE m.status=2 AND m.created_at<? AND NOT EXISTS("
      "SELECT 1 FROM messages WHERE messages.media_id=m.id) "
      "ORDER BY m.created_at ASC LIMIT " +
      std::to_string(limit);
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql, {std::to_string(olderThanSec)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 查询孤儿媒体失败 errno=" << errNo);
    return out;
  }
  MediaRowBuf b;
  bindMediaResult(stmt, b);
  if (mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 孤儿媒体结果存储失败: " << client_.errStr());
    mysql_stmt_close(stmt);
    return out;
  }
  while (mysql_stmt_fetch(stmt) == 0) out.push_back(rowToMedia(b));
  mysql_stmt_close(stmt);
  return out;
}

bool MySqlMediaRepository::markOrphanGc(const std::string& fileId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  // 仅 COMMITTED(2)→GC(3)：防与 msg.send（只认 COMMITTED）竞态误删引用中对象
  const std::string sql = "UPDATE media SET status=3 WHERE file_id=? AND status=2";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(sql, {fileId}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 标记孤儿媒体失败 errno=" << errNo);
    return false;
  }
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  return affected > 0;
}

bool MySqlMediaRepository::revertGc(const std::string& fileId, int toStatus) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  // 仅 GC(3)→toStatus 回滚：状态已被其它路径改写时不覆盖
  const std::string sql = "UPDATE media SET status=? WHERE file_id=? AND status=3";
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      sql, {std::to_string(toStatus), fileId}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 回滚媒体状态失败 errno=" << errNo);
    return false;
  }
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  return affected > 0;
}

int MySqlMediaRepository::isReferenced(const std::string& fileId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return -1;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT 1 FROM messages WHERE media_id=(SELECT id FROM media WHERE file_id=?) "
      "LIMIT 1",
      {fileId}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 媒体引用查询失败 errno=" << errNo);
    return -1;  // 查询失败：返回未知，禁止当作「未引用」以免 GC 误删
  }
  MYSQL_BIND res[1];
  int dummy = 0;
  bool dummyNull = false;
  unsigned long dummyLen = 0;
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONG;
  res[0].buffer = &dummy;
  res[0].is_null = &dummyNull;
  res[0].length = &dummyLen;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    LOG_E("[mysql] 媒体引用结果读取失败: " << client_.errStr());
    mysql_stmt_close(stmt);
    return -1;  // 结果读取异常：状态未知
  }
  const int fetched = mysql_stmt_fetch(stmt);
  mysql_stmt_close(stmt);
  if (fetched == 0) return 1;   // 查到行 → 被引用
  if (fetched == MYSQL_NO_DATA) return 0;  // 无行 → 确定未引用
  LOG_E("[mysql] 媒体引用 fetch 失败");
  return -1;  // fetch 异常 → 状态未知
}

}  // namespace im
