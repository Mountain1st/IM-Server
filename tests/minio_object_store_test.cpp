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

int main() {
  RUN(minio_presign_put_and_get_shape);
  RUN(minio_disabled_when_config_missing);
  std::printf("minio_object_store_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
