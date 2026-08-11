#include "util.hpp"

#include <array>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <cstdio>
#include <cstring>

namespace im {

bool setNonBlocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags == -1) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool setNoDelay(int fd) {
  int one = 1;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
}

bool setReuseAddr(int fd) {
  int one = 1;
  return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0;
}

bool blockStopSignals() {
  sigset_t set;
  ::sigemptyset(&set);
  ::sigaddset(&set, SIGINT);
  ::sigaddset(&set, SIGTERM);
  return ::pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0;
}

namespace {

inline uint32_t rol32(uint32_t v, int b) {
  return (v << b) | (v >> (32 - b));
}

} // namespace

std::string sha1Raw(const std::string& input) {
  auto raw = sha1Hex(input);
  std::string out;
  out.reserve(20);
  for (size_t i = 0; i < raw.size(); i += 2) {
    auto hb = std::stoi(raw.substr(i, 2), nullptr, 16);
    out.push_back(static_cast<char>(hb));
  }
  return out;
}

std::string sha1Hex(const std::string& input) {
  std::array<uint32_t, 5> h = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

  // 填充
  std::string msg = input;
  uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0x00);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFF));

  for (size_t i = 0; i < msg.size(); i += 64) {
    std::array<uint32_t, 80> w{};
    for (int j = 0; j < 16; ++j) {
      const unsigned char* p =
        reinterpret_cast<const unsigned char*>(msg.data() + i) + j * 4;
      w[j] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
             (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    for (int j = 16; j < 80; ++j) {
      w[j] = rol32(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int j = 0; j < 80; ++j) {
      uint32_t f, k;
      if (j < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
      else if (j < 40) { f = b ^ c ^ d;           k = 0x6ED9EBA1; }
      else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else             { f = b ^ c ^ d;           k = 0xCA62C1D6; }
      uint32_t tmp = rol32(a, 5) + f + e + k + w[j];
      e = d; d = c; c = rol32(b, 30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }

  std::string out;
  out.reserve(40);
  for (auto x : h) {
    char buf[9];
    snprintf(buf, sizeof buf, "%08x", x);
    out += buf;
  }
  return out;
}

std::string base64Encode(const std::string& input) {
  static const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < input.size()) {
    uint32_t n = (uint32_t(uint8_t(input[i])) << 16) |
          (uint32_t(uint8_t(input[i + 1])) << 8) |
          uint32_t(uint8_t(input[i + 2]));
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += table[(n >> 6) & 0x3F];
    out += table[n & 0x3F];
    i += 3;
  }
  size_t rem = input.size() - i;
  if (rem == 1) {
    uint32_t n = uint32_t(uint8_t(input[i])) << 16;
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += "==";
  } else if (rem == 2) {
    uint32_t n = (uint32_t(uint8_t(input[i])) << 16) |
          (uint32_t(uint8_t(input[i + 1])) << 8);
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += table[(n >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

} // namespace im