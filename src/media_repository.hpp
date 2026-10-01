#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace im {

struct MediaRecord {
  int64_t id = 0;
  std::string fileId;
  int64_t uploaderId = 0;
  std::string objectKey;
  std::string fileName;
  std::string mimeType;
  int64_t fileSize = 0;
  int width = 0, height = 0, duration = 0;
  std::string thumbKey;
  int status = 0;          // 0 PENDING 1 UPLOADED 2 COMMITTED 3 GC
  int64_t createdAt = 0;
};

enum MediaStatus { kMediaPending = 0, kMediaUploaded = 1, kMediaCommitted = 2, kMediaGc = 3 };

class MediaRepository {
 public:
  virtual ~MediaRepository() = default;
  virtual bool ok() const = 0;
  virtual bool insertPending(const MediaRecord& m) = 0;
  virtual std::optional<MediaRecord> findByFileId(const std::string& fileId) = 0;
  // commit：PENDING→COMMITTED，写入以 HEAD 为准的 size 与元数据；幂等（已 COMMITTED 返回 true）
  virtual bool commit(const std::string& fileId, int64_t size, int width, int height, int duration) = 0;
  virtual bool markDeleted(const std::string& fileId) = 0;
  // GC：返回超期 PENDING（< olderThan）或已删记录，供清理对象
  virtual std::vector<MediaRecord> findPendingOlderThan(int64_t olderThanSec, int limit) = 0;
  // 是否被任何消息引用
  virtual bool isReferenced(const std::string& fileId) = 0;
};
} // namespace im
