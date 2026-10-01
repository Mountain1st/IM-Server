#include "test_util.hpp"

#include "media_store.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace im;

// 1x1 PNG（透明），标准测试向量
static const char* kPng1x1B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJ"
    "RU5ErkJggg==";

TEST(media_png_1x1_store_ok) {
  auto ok = MediaStore::decodeAndStore(kPng1x1B64, "/tmp/uploads-test");
  CHECK(ok.has_value());
  if (ok.has_value()) {
    // 形如 /uploads/<64hex>.png
    CHECK(ok->rfind("/uploads/", 0) == 0);
    CHECK(ok->size() == 9 + 64 + 4);
    CHECK(ok->substr(ok->size() - 4) == ".png");
    std::string path = std::string("/tmp/uploads-test/") + ok->substr(9);
    FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    if (f != nullptr) std::fclose(f);
  }
}

TEST(media_invalid_b64_reject) {
  CHECK(!MediaStore::isValidBase64("not-base64!!!"));
  CHECK(!MediaStore::decodeAndStore("not-base64!!!", "/tmp/uploads-test").has_value());
  CHECK(!MediaStore::isValidBase64(""));
}

TEST(media_svg_reject) {
  const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>";
  auto bytes = std::vector<uint8_t>(svg.begin(), svg.end());
  CHECK(MediaStore::detectKind(bytes) == MediaKind::kReject);
  // base64 后的 svg 经 decodeAndStore 也应拒绝
  auto enc = MediaStore::b64EncodeForTest(svg);
  CHECK(!MediaStore::decodeAndStore(enc, "/tmp/uploads-test").has_value());
}

TEST(media_oversize_dimension_reject) {
  // 构造 PNG 头：签名 + IHDR(宽 5000，高 1)，仅测维度拒绝逻辑
  std::vector<uint8_t> bytes = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
                                0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
                                0x00, 0x00, 0x13, 0x88, 0x00, 0x00, 0x00, 0x01,
                                0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  int w = 0;
  int h = 0;
  CHECK(MediaStore::detectKind(bytes, &w, &h) == MediaKind::kReject);
  CHECK(w == 5000);
  CHECK(h == 1);
}

TEST(media_png_kind_and_size) {
  auto raw = MediaStore::b64Decode(kPng1x1B64);
  CHECK(raw.has_value());
  int w = 0;
  int h = 0;
  CHECK(MediaStore::detectKind(*raw, &w, &h) == MediaKind::kPng);
  CHECK(w == 1);
  CHECK(h == 1);
}

int main() {
  RUN(media_png_1x1_store_ok);
  RUN(media_invalid_b64_reject);
  RUN(media_svg_reject);
  RUN(media_oversize_dimension_reject);
  RUN(media_png_kind_and_size);
  std::printf("media_store_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
