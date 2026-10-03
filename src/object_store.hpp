#pragma once
#include <cstdint>
#include <string>
#include <vector>
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
  // 下载对象前缀（服务端预签名 GET + libcurl，读满 maxBytes 即截断视为成功）。
  // 成功返回 true 并写 out；对象不存在/网络失败返回 false。
  // commit 内容校验用（只读头部字节，不做完整下载）。
  virtual bool getObject(const std::string& key, size_t maxBytes,
                         std::vector<uint8_t>* out) = 0;
};
} // namespace im
