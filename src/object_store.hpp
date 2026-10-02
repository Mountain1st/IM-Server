#pragma once
#include <cstdint>
#include <string>
namespace im {

struct PresignedUrl { std::string url; int64_t expiresIn = 0; };
struct ObjectInfo { bool exists = false; uint64_t size = 0; };

class ObjectStore {
 public:
  virtual ~ObjectStore() = default;
  virtual bool enabled() const = 0;
  // ttlSec 由调用方给（PUT 900s / GET 3600s）；amzDate 便于单测固定时钟
  virtual PresignedUrl presignPut(const std::string& key, int64_t ttlSec,
                                  const std::string& amzDate = "") = 0;
  virtual PresignedUrl presignGet(const std::string& key, int64_t ttlSec,
                                  const std::string& amzDate = "") = 0;
  virtual ObjectInfo headObject(const std::string& key) = 0;   // 服务端预签名+libcurl
  virtual bool deleteObject(const std::string& key) = 0;       // 服务端预签名+libcurl
};
} // namespace im
