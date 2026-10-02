// tests/crypto_test.cpp
#include "test_util.hpp"
#include "util.hpp"
#include <string>
using namespace im;

TEST(sha256_fips_vectors) {
  CHECK_EQ(sha256Hex(""),
      std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha256Hex("abc"),
      std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(hmac_sha256_rfc4231) {
  // TC1: key=20B 0x0b, data="Hi There"
  // 期望含 \x00 字节，必须用显式长度构造，避免 std::string(const char*) 在 NUL 处截断
  std::string k1(20, (char)0x0b);
  CHECK_EQ(hmacSha256Raw(k1, "Hi There"),
      std::string("\xb0\x34\x4c\x61\xd8\xdb\x38\x53\x5c\xa8\xaf\xce\xaf\x0b\xf1\x2b"
                  "\x88\x1d\xc2\x00\xc9\x83\x3d\xa7\x26\xe9\x37\x6c\x2e\x32\xcf\xf7", 32));
  // TC3: key=20B 0xaa, data=50B 0xdd
  std::string k3(20, (char)0xaa), d3(50, (char)0xdd);
  CHECK_EQ(hmacSha256Raw(k3, d3),
      std::string("\x77\x3e\xa9\x1e\x36\x80\x0e\x46\x85\x4d\xb8\xeb\xd0\x91\x81\xa7"
                  "\x29\x59\x09\x8b\x3e\xf8\xc1\x22\xd9\x63\x55\x14\xce\xd5\x65\xfe"));
}

TEST(hmac_sha256_binary_key) {
  // SigV4 派生密钥是 32B 二进制（含 NUL），必须不作字符串截断
  std::string key;
  for (int i = 0; i < 32; ++i) key.push_back(static_cast<char>(i));
  auto a = hmacSha256Raw(key, "x");
  CHECK_EQ(a.size(), size_t(32));
  CHECK_EQ(a, hmacSha256Raw(key, "x"));
}

int main() {
  RUN(sha256_fips_vectors);
  RUN(hmac_sha256_rfc4231);
  RUN(hmac_sha256_binary_key);
  std::printf("crypto_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
