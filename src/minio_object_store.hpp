#pragma once
#include "object_store.hpp"

#include <string>
#include <vector>

namespace im {

struct MinioConfig {
  std::string endpointHost;  // 不含 scheme（服务端内网访问，如 minio:9000）
  std::string scheme = "http";
  std::string region = "us-east-1";
  std::string bucket;
  std::string accessKey;
  std::string secretKey;
  bool pathStyle = true;
  // 客户端可达的公网地址（不含 scheme）：预签名 PUT/GET 面向外部客户端，
  // 必须用公网域名签名（SigV4 签名覆盖 Host）；留空=回落 endpointHost。
  // 服务端操作（HEAD/DELETE/commit 校验 GET）始终用内网 endpointHost。
  std::string publicHost;
  std::string publicScheme = "https";  // 公网 scheme（publicHost 非空时生效）
};

class MinioObjectStore : public ObjectStore {
 public:
  explicit MinioObjectStore(MinioConfig c);
  bool enabled() const override { return enabled_; }
  PresignedUrl presignPut(const std::string& key, int64_t ttlSec,
                          const std::string& amzDate = "") override;
  PresignedUrl presignGet(const std::string& key, int64_t ttlSec,
                          const std::string& amzDate = "") override;
  ObjectInfo headObject(const std::string& key) override;
  bool deleteObject(const std::string& key) override;
  bool getObject(const std::string& key, size_t maxBytes,
                 std::vector<uint8_t>* out) override;

 private:
  MinioConfig cfg_;
  bool enabled_ = false;
  static std::string nowAmzDate();  // UTC yyyyMMddTHHmmssZ
};

} // namespace im
