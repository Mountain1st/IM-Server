#include "minio_object_store.hpp"

#include "sigv4.hpp"

#include <curl/curl.h>

#include <ctime>
#include <mutex>

namespace im {
namespace {

// 进程级一次初始化 libcurl（线程安全）
void ensureCurlInit() {
  static std::once_flag flag;
  std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// 丢弃响应体（HEAD 无 body；DELETE 有 body 也无需解析）
size_t discardBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
  (void)ptr;
  (void)userdata;
  return size * nmemb;
}

// MinioConfig → SigV4Config（两者字段同源，pathStyle/scheme 直通）
SigV4Config toSigV4(const MinioConfig& c) {
  SigV4Config s;
  s.host = c.endpointHost;
  s.region = c.region;
  s.bucket = c.bucket;
  s.accessKey = c.accessKey;
  s.secretKey = c.secretKey;
  s.pathStyle = c.pathStyle;
  s.scheme = c.scheme;
  return s;
}

} // namespace

MinioObjectStore::MinioObjectStore(MinioConfig c) : cfg_(std::move(c)) {
  enabled_ = !cfg_.endpointHost.empty() && !cfg_.bucket.empty() &&
             !cfg_.accessKey.empty() && !cfg_.secretKey.empty();
}

std::string MinioObjectStore::nowAmzDate() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return std::string(buf);
}

PresignedUrl MinioObjectStore::presignPut(const std::string& key, int64_t ttlSec,
                                          const std::string& amzDate) {
  PresignedUrl out;
  if (!enabled_) return out;
  const std::string date = amzDate.empty() ? nowAmzDate() : amzDate;
  SigV4Request req{"PUT", key, date, ttlSec};
  out.url = sigV4Presign(toSigV4(cfg_), req);
  out.expiresIn = out.url.empty() ? 0 : ttlSec;
  return out;
}

PresignedUrl MinioObjectStore::presignGet(const std::string& key, int64_t ttlSec,
                                          const std::string& amzDate) {
  PresignedUrl out;
  if (!enabled_) return out;
  const std::string date = amzDate.empty() ? nowAmzDate() : amzDate;
  SigV4Request req{"GET", key, date, ttlSec};
  out.url = sigV4Presign(toSigV4(cfg_), req);
  out.expiresIn = out.url.empty() ? 0 : ttlSec;
  return out;
}

ObjectInfo MinioObjectStore::headObject(const std::string& key) {
  ObjectInfo info;
  if (!enabled_) return info;
  // SigV4 签名覆盖 HTTP 方法：HEAD 必须以 HEAD 方法签名，用 GET URL 发 HEAD 会 403
  SigV4Request req{"HEAD", key, nowAmzDate(), 300};
  const std::string url = sigV4Presign(toSigV4(cfg_), req);
  if (url.empty()) return info;

  ensureCurlInit();
  CURL* curl = curl_easy_init();
  if (!curl) return info;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "HEAD");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardBody);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (httpCode == 200) {
      curl_off_t len = -1;
      curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
      info.exists = true;
      info.size = len > 0 ? static_cast<uint64_t>(len) : 0;
    }
  }
  curl_easy_cleanup(curl);
  return info;
}

bool MinioObjectStore::deleteObject(const std::string& key) {
  if (!enabled_) return false;
  const std::string date = nowAmzDate();
  SigV4Request req{"DELETE", key, date, 300};
  const std::string url = sigV4Presign(toSigV4(cfg_), req);
  if (url.empty()) return false;

  ensureCurlInit();
  CURL* curl = curl_easy_init();
  if (!curl) return false;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardBody);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  bool ok = false;
  if (rc == CURLE_OK) {
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    ok = httpCode >= 200 && httpCode < 300;
  }
  curl_easy_cleanup(curl);
  return ok;
}

} // namespace im
