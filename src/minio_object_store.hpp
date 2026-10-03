#pragma once
#include "object_store.hpp"

#include <string>
#include <vector>

namespace im {

struct MinioConfig {
  std::string endpointHost;  // 不含 scheme
  std::string scheme = "http";
  std::string region = "us-east-1";
  std::string bucket;
  std::string accessKey;
  std::string secretKey;
  bool pathStyle = true;
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
