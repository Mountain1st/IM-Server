#pragma once

#include "media_repository.hpp"
#include "mysql_client.hpp"

#include <mutex>

namespace im {

// 媒体元数据的 MySQL 实现（media 表）。
// MySQL 单连接非线程安全：内部用互斥锁串行化所有 DB 访问。
class MySqlMediaRepository : public MediaRepository {
 public:
  MySqlMediaRepository(const std::string& host, unsigned port,
                       const std::string& user, const std::string& password,
                       const std::string& db);

  bool ok() const override { return client_.ok(); }
  bool insertPending(const MediaRecord& m) override;
  std::optional<MediaRecord> findByFileId(const std::string& fileId) override;
  // commit：PENDING→COMMITTED，写入以 HEAD 为准的元数据；已 COMMITTED 视为成功（幂等）
  bool commit(const std::string& fileId, int64_t size, int width, int height,
              int duration) override;
  bool markDeleted(const std::string& fileId) override;
  // 返回 createdAt < olderThanSec 的 PENDING 记录（供 GC 清理对象），limit 条。
  // 注意：olderThanSec 实为「绝对时间戳上界」，调用方须传 now - ttlSec。
  std::vector<MediaRecord> findPendingOlderThan(int64_t olderThanSec,
                                                int limit) override;
  // 1=被引用，0=确定未引用，-1=查询失败（未知）
  int isReferenced(const std::string& fileId) override;

 private:
  MySqlClient client_;
  std::mutex mutex_;  // 串行化 DB 访问（单连接非线程安全）
};

}  // namespace im
