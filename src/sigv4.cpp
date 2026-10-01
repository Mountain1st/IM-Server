#include "sigv4.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace im {
namespace {
bool isUnreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

// 派生签名密钥：HMAC 链，中间值是二进制，不能转 hex
std::string signingKey(const std::string& secret, const std::string& date,
                       const std::string& region, const std::string& service) {
  std::string k1 = hmacSha256Raw("AWS4" + secret, date);
  std::string k2 = hmacSha256Raw(k1, region);
  std::string k3 = hmacSha256Raw(k2, service);
  return hmacSha256Raw(k3, "aws4_request");
}

std::string toHex(const std::string& raw) {
  static const char* h = "0123456789abcdef";
  std::string o;
  for (unsigned char c : raw) { o += h[c >> 4]; o += h[c & 0xF]; }
  return o;
}
} // namespace

std::string sigV4UriEncode(const std::string& s, bool encodeSlash) {
  static const char* h = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isUnreserved(c) || (c == '/' && !encodeSlash)) {
      o.push_back(static_cast<char>(c));
    } else {
      o.push_back('%'); o.push_back(h[c >> 4]); o.push_back(h[c & 0xF]);
    }
  }
  return o;
}

std::string sigV4DateStamp(const std::string& amzDate) {
  return amzDate.size() >= 8 ? amzDate.substr(0, 8) : std::string();
}

std::string sigV4Presign(const SigV4Config& cfg, const SigV4Request& req) {
  if (cfg.host.empty() || cfg.region.empty() || cfg.bucket.empty() ||
      cfg.accessKey.empty() || cfg.secretKey.empty()) return "";
  if (req.method != "GET" && req.method != "PUT" &&
      req.method != "HEAD" && req.method != "DELETE") return "";
  if (req.expiresSec <= 0 || req.expiresSec > 604800) return "";
  const std::string dateStamp = sigV4DateStamp(req.amzDate);
  if (dateStamp.empty() || req.amzDate.size() != 16) return "";

  const std::string service = "s3";
  // Canonical URI：每个 path 段编码，保留 '/'
  std::string canonicalUri = "/" + sigV4UriEncode(cfg.bucket, true) + "/" +
                             sigV4UriEncode(req.objectKey, false);
  // query 参数（按 key 排序：X-Amz-* 自然有序）
  const std::string scope = dateStamp + "/" + cfg.region + "/" + service + "/aws4_request";
  std::vector<std::pair<std::string, std::string>> q = {
      {"X-Amz-Algorithm", "AWS4-HMAC-SHA256"},
      {"X-Amz-Credential", sigV4UriEncode(cfg.accessKey + "/" + scope, true)},
      {"X-Amz-Date", req.amzDate},
      {"X-Amz-Expires", std::to_string(req.expiresSec)},
      {"X-Amz-SignedHeaders", "host"},
  };
  std::sort(q.begin(), q.end());
  std::string canonicalQuery;
  for (size_t i = 0; i < q.size(); ++i) {
    if (i) canonicalQuery += "&";
    canonicalQuery += sigV4UriEncode(q[i].first, true) + "=" + q[i].second;
  }
  const std::string canonicalHeaders = "host:" + cfg.host + "\n";
  const std::string canonicalRequest =
      req.method + "\n" + canonicalUri + "\n" + canonicalQuery + "\n" +
      canonicalHeaders + "\n" + "host" + "\n" + "UNSIGNED-PAYLOAD";
  const std::string stringToSign =
      "AWS4-HMAC-SHA256\n" + req.amzDate + "\n" + scope + "\n" +
      sha256Hex(canonicalRequest);
  const std::string signature =
      toHex(hmacSha256Raw(signingKey(cfg.secretKey, dateStamp, cfg.region, service),
                          stringToSign));
  return cfg.scheme + "://" + cfg.host + canonicalUri + "?" + canonicalQuery +
         "&X-Amz-Signature=" + signature;
}

} // namespace im
