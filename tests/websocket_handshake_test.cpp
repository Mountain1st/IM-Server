#include "test_util.hpp"

#include "websocket.hpp"

#include <string>

using namespace im;

namespace {

std::string buildRequest(const std::string& firstLine,
                         const std::vector<std::string>& headers) {
  std::string out = firstLine + "\r\n";
  for (const auto& h : headers) out += h + "\r\n";
  out += "\r\n";
  return out;
}

std::string validKey = "dGhlIHNhbXBsZSBub25jZQ=="; // 16 字节 Base64

// 标准合法请求头
std::vector<std::string> baseHeaders() {
  return {
      "Host: 127.0.0.1:9191",
      "Upgrade: websocket",
      "Connection: Upgrade",
      "Sec-WebSocket-Key: " + validKey,
      "Sec-WebSocket-Version: 13",
  };
}

} // namespace

TEST(handshake_ok) {
  auto req = buildRequest("GET / HTTP/1.1", baseHeaders());
  std::string key;
  std::string reason;
  CHECK(ws::validateHandshake(req, &key, &reason));
  CHECK_EQ(key, validKey);
}

TEST(handshake_header_case_insensitive) {
  auto req = buildRequest("GET / HTTP/1.1",
      {"host: 127.0.0.1:9191", "upgrade: websocket", "connection: upgrade",
       "sec-websocket-key: " + validKey, "sec-websocket-version: 13"});
  std::string key, reason;
  CHECK(ws::validateHandshake(req, &key, &reason));
}

TEST(handshake_connection_multi_token) {
  auto req = buildRequest("GET / HTTP/1.1",
      {"Host: x", "Upgrade: websocket", "Connection: keep-alive, Upgrade",
       "Sec-WebSocket-Key: " + validKey, "Sec-WebSocket-Version: 13"});
  std::string key, reason;
  CHECK(ws::validateHandshake(req, &key, &reason));
}

TEST(handshake_wrong_method_rejected) {
  auto req = buildRequest("POST / HTTP/1.1", baseHeaders());
  std::string key, reason;
  CHECK(!ws::validateHandshake(req, &key, &reason));
}

TEST(handshake_wrong_path_rejected) {
  auto req = buildRequest("GET /chat HTTP/1.1", baseHeaders());
  std::string key, reason;
  CHECK(!ws::validateHandshake(req, &key, &reason));
}

TEST(handshake_wrong_version_rejected) {
  auto req = buildRequest("GET / HTTP/1.0", baseHeaders());
  std::string key, reason;
  CHECK(!ws::validateHandshake(req, &key, &reason));
}

TEST(handshake_missing_host_rejected) {
  auto req = buildRequest("GET / HTTP/1.1", {
      "Upgrade: websocket", "Connection: Upgrade",
      "Sec-WebSocket-Key: " + validKey, "Sec-WebSocket-Version: 13"});
  std::string key, reason;
  CHECK(!ws::validateHandshake(req, &key, &reason));
}

TEST(handshake_bad_key_format_rejected) {
  auto req = buildRequest("GET / HTTP/1.1", {
      "Host: x", "Upgrade: websocket", "Connection: Upgrade",
      "Sec-WebSocket-Key: not-a-base64-key", "Sec-WebSocket-Version: 13"});
  std::string key, reason;
  CHECK(!ws::validateHandshake(req, &key, &reason));
}

TEST(handshake_missing_upgrade_rejected) {
  auto req = buildRequest("GET / HTTP/1.1", {
      "Host: x", "Connection: Upgrade",
      "Sec-WebSocket-Key: " + validKey, "Sec-WebSocket-Version: 13"});
  std::string key, reason;
  CHECK(!ws::validateHandshake(req, &key, &reason));
}

TEST(handshake_wrong_version_header_rejected) {
  auto req = buildRequest("GET / HTTP/1.1", {
      "Host: x", "Upgrade: websocket", "Connection: Upgrade",
      "Sec-WebSocket-Key: " + validKey, "Sec-WebSocket-Version: 12"});
  std::string key, reason;
  CHECK(!ws::validateHandshake(req, &key, &reason));
}

int main() {
  RUN(handshake_ok);
  RUN(handshake_header_case_insensitive);
  RUN(handshake_connection_multi_token);
  RUN(handshake_wrong_method_rejected);
  RUN(handshake_wrong_path_rejected);
  RUN(handshake_wrong_version_rejected);
  RUN(handshake_missing_host_rejected);
  RUN(handshake_bad_key_format_rejected);
  RUN(handshake_missing_upgrade_rejected);
  RUN(handshake_wrong_version_header_rejected);
  std::printf("websocket_handshake_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}