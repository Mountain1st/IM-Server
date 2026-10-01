#pragma once
#include <cstdint>
#include <string>
namespace im {

struct SigV4Config {
  std::string host;        // endpoint host，如 minio.example.com（不含 scheme）
  std::string region;      // 如 us-east-1
  std::string bucket;
  std::string accessKey;
  std::string secretKey;
  bool pathStyle = true;   // MinIO 用 path style
  std::string scheme = "http"; // 生产 https，本地 http
};

struct SigV4Request {
  std::string method;      // GET/PUT/HEAD/DELETE
  std::string objectKey;   // 不含 bucket，不含前导 /
  std::string amzDate;     // UTC yyyyMMddTHHmmssZ
  int64_t expiresSec;      // 1..604800
};

// 生成预签名 URL；参数非法（method/expires/空字段）返回空串。
// 纯函数，无副作用，可单测。
std::string sigV4Presign(const SigV4Config& cfg, const SigV4Request& req);

// 单测辅助：AWS/S3 URI 编码（对 unreserved 集外的字符做 %XX，空格→%20）
std::string sigV4UriEncode(const std::string& s, bool encodeSlash);

// 单测辅助：给定 UTC yyyyMMddTHHmmssZ 取 dateStamp(yyyyMMdd)
std::string sigV4DateStamp(const std::string& amzDate);

} // namespace im
