#include "test_util.hpp"
#include "sigv4.hpp"
using namespace im;

TEST(sigv4_presign_get_deterministic) {
  SigV4Config cfg{"minio.example.com", "us-east-1", "mybucket",
                  "AKIAIOSFODNN7EXAMPLE",
                  "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", true};
  SigV4Request req{"GET", "media/1001/2026/10/ab.jpg",
                   /*amzDate=*/"20261001T120000Z", /*expiresSec=*/900};
  auto url = sigV4Presign(cfg, req);
  // 确定性：同输入同输出，且含必备 query
  CHECK(url.find("X-Amz-Algorithm=AWS4-HMAC-SHA256") != std::string::npos);
  CHECK(url.find("X-Amz-Credential=") != std::string::npos);
  CHECK(url.find("X-Amz-Date=20261001T120000Z") != std::string::npos);
  CHECK(url.find("X-Amz-Expires=900") != std::string::npos);
  CHECK(url.find("X-Amz-SignedHeaders=host") != std::string::npos);
  CHECK(url.find("X-Amz-Signature=") != std::string::npos);
  CHECK_EQ(url, sigV4Presign(cfg, req));
}

TEST(sigv4_known_answer) {
  // KAT：固定输入写死完整签名 URL，防止 canonical request 组装错误仍自洽通过。
  // 常量由 AWS SigV4 规范用 Python3 hashlib/hmac 独立复算，与 C++ 输出逐字节一致。
  SigV4Config cfg{"minio.example.com", "us-east-1", "mybucket",
                  "AKIAIOSFODNN7EXAMPLE",
                  "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", true, "http"};
  SigV4Request req{"GET", "media/1001/2026/10/test.png",
                   /*amzDate=*/"20261001T120000Z", /*expiresSec=*/900};
  const std::string expected =
      "http://minio.example.com/mybucket/media/1001/2026/10/test.png"
      "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
      "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20261001%2Fus-east-1"
      "%2Fs3%2Faws4_request"
      "&X-Amz-Date=20261001T120000Z"
      "&X-Amz-Expires=900"
      "&X-Amz-SignedHeaders=host"
      "&X-Amz-Signature="
      "797897c96c275d3e7e7ec179fe8a93715849fac10669808b875d24776047c66b";
  CHECK_EQ(sigV4Presign(cfg, req), expected);
}

TEST(sigv4_utf8_key_encoding) {
  SigV4Config cfg{"h", "us-east-1", "b", "AK", "SK", true};
  SigV4Request req{"PUT", "media/1001/相册 图.jpg", "20261001T120000Z", 900};
  auto url = sigV4Presign(cfg, req);
  // 空格必须 %20 不是 +；UTF-8 逐字节百分号编码
  CHECK(url.find("%20") != std::string::npos);
  CHECK(url.find('+') == std::string::npos);
  CHECK(url.find("%E7%9B%B8") != std::string::npos); // "相"
}

TEST(sigv4_scope_and_host) {
  SigV4Config cfg{"minio.example.com", "us-east-1", "mybucket", "AK", "SK", true};
  SigV4Request req{"PUT", "k.png", "20261001T120000Z", 600};
  auto url = sigV4Presign(cfg, req);
  CHECK(url.find("s3%2Faws4_request") != std::string::npos); // Credential scope
  CHECK(url.rfind("http://minio.example.com/mybucket/k.png", 0) == 0); // path style
}

TEST(sigv4_expires_overflow_rejected) {
  SigV4Config cfg{"h", "us-east-1", "b", "AK", "SK", true};
  SigV4Request req{"GET", "k", "20261001T120000Z", 604801}; // >7 天上限
  CHECK(sigV4Presign(cfg, req).empty());
}

TEST(sigv4_expires_zero_rejected) {
  SigV4Config cfg{"h", "us-east-1", "b", "AK", "SK", true};
  SigV4Request req{"GET", "k", "20261001T120000Z", 0}; // 下界 0 拒绝
  CHECK(sigV4Presign(cfg, req).empty());
}

TEST(sigv4_invalid_method_rejected) {
  SigV4Config cfg{"h", "us-east-1", "b", "AK", "SK", true};
  SigV4Request req{"POST", "k", "20261001T120000Z", 900}; // 仅支持 GET/PUT/HEAD/DELETE
  CHECK(sigV4Presign(cfg, req).empty());
}

TEST(sigv4_empty_secret_rejected) {
  SigV4Config cfg{"h", "us-east-1", "b", "AK", "", true}; // 空 secretKey
  SigV4Request req{"GET", "k", "20261001T120000Z", 900};
  CHECK(sigV4Presign(cfg, req).empty());
}

int main() {
  RUN(sigv4_presign_get_deterministic);
  RUN(sigv4_known_answer);
  RUN(sigv4_utf8_key_encoding);
  RUN(sigv4_scope_and_host);
  RUN(sigv4_expires_overflow_rejected);
  RUN(sigv4_expires_zero_rejected);
  RUN(sigv4_invalid_method_rejected);
  RUN(sigv4_empty_secret_rejected);
  std::printf("sigv4_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
