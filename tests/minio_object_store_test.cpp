// tests/minio_object_store_test.cpp —— 纯逻辑（不连真机）
#include "test_util.hpp"
#include "minio_object_store.hpp"
using namespace im;

TEST(minio_presign_put_and_get_shape) {
  MinioConfig c;
  c.endpointHost = "minio.example.com"; c.region = "us-east-1";
  c.bucket = "media"; c.accessKey = "AK"; c.secretKey = "SK"; c.pathStyle = true;
  MinioObjectStore s(c);
  auto put = s.presignPut("media/1/a.png", 900, "20261001T120000Z");
  auto get = s.presignGet("media/1/a.png", 3600, "20261001T120000Z");
  CHECK(put.url.rfind("http://minio.example.com/media/media/1/a.png?", 0) == 0);
  CHECK(put.url.find("X-Amz-Expires=900") != std::string::npos);
  CHECK(get.url.find("X-Amz-Expires=3600") != std::string::npos);
  CHECK(put.expiresIn == 900 && get.expiresIn == 3600);
}

TEST(minio_disabled_when_config_missing) {
  MinioConfig c; // 空配置
  MinioObjectStore s(c);
  CHECK(!s.enabled());
  CHECK(s.presignPut("k", 900, "20261001T120000Z").url.empty());
}

TEST(minio_presign_uses_public_endpoint_when_configured) {
  // 任务⑤：客户端直传/直取的预签名 URL 必须用公网 host（Host 参与 SigV4 签名），
  // 否则外部客户端拿到 minio:9000 内网地址不可达（审计 P0-1）
  MinioConfig c;
  c.endpointHost = "minio:9000"; c.region = "us-east-1";
  c.bucket = "media"; c.accessKey = "AK"; c.secretKey = "SK"; c.pathStyle = true;
  c.publicHost = "oss.example.com"; c.publicScheme = "https";
  MinioObjectStore s(c);
  const auto put = s.presignPut("media/1/a.png", 900, "20261001T120000Z");
  const auto get = s.presignGet("media/1/a.png", 3600, "20261001T120000Z");
  CHECK(put.url.rfind("https://oss.example.com/media/media/1/a.png?", 0) == 0);
  CHECK(get.url.rfind("https://oss.example.com/media/media/1/a.png?", 0) == 0);
  // 签名覆盖 Host：URL 中不得残留内网 endpoint
  CHECK(put.url.find("minio:9000") == std::string::npos);
}

TEST(minio_presign_falls_back_to_internal_endpoint) {
  // 未配公网时回落内网（本地/CI 同网段；行为与既有测试一致）
  MinioConfig c;
  c.endpointHost = "127.0.0.1:9000"; c.region = "us-east-1";
  c.bucket = "media"; c.accessKey = "AK"; c.secretKey = "SK"; c.pathStyle = true;
  MinioObjectStore s(c);
  const auto put = s.presignPut("media/1/a.png", 900, "20261001T120000Z");
  CHECK(put.url.rfind("http://127.0.0.1:9000/media/media/1/a.png?", 0) == 0);
}

int main() {
  RUN(minio_presign_put_and_get_shape);
  RUN(minio_disabled_when_config_missing);
  RUN(minio_presign_uses_public_endpoint_when_configured);
  RUN(minio_presign_falls_back_to_internal_endpoint);
  std::printf("minio_object_store_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
