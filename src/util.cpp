// ============================================================
// util：通用工具（协议层支撑）
// - PBKDF2-SHA256（本文件手写 SHA256/HMAC-SHA256 实现，不依赖 OpenSSL）+ 恒定时间比较（防时序侧信道）
// - 随机 token 生成（256 位熵）与摘取
// - validAccount/validPassword：注册参数格式校验
// - blockStopSignals：工作线程屏蔽 SIGINT/SIGTERM（信号归主线程）
// - 非阻塞/无延迟 socket 选项设置
// ============================================================
#include "util.hpp"

#include <array>
#include <csignal>
#include <iomanip>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sstream>
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

namespace {

// ---- SHA-256（FIPS 180-4），供 HMAC-SHA256 / PBKDF2 内部使用 ----

inline uint32_t rotr32(uint32_t v, int b) {
  return (v >> b) | (v << (32 - b));
}

// 计算 SHA-256 摘要，返回 32 字节原始值
std::string sha256Raw(const std::string& input) {
  static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  };
  std::array<uint32_t, 8> h = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  };

  std::string msg = input;
  uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0x00);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFF));

  for (size_t i = 0; i < msg.size(); i += 64) {
    std::array<uint32_t, 64> w{};
    for (int j = 0; j < 16; ++j) {
      const unsigned char* p =
        reinterpret_cast<const unsigned char*>(msg.data() + i) + j * 4;
      w[j] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
             (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    for (int j = 16; j < 64; ++j) {
      uint32_t s0 = rotr32(w[j - 15], 7) ^ rotr32(w[j - 15], 18) ^ (w[j - 15] >> 3);
      uint32_t s1 = rotr32(w[j - 2], 17) ^ rotr32(w[j - 2], 19) ^ (w[j - 2] >> 10);
      w[j] = w[j - 16] + s0 + w[j - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int j = 0; j < 64; ++j) {
      uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = hh + S1 + ch + K[j] + w[j];
      uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = S0 + maj;
      hh = g; g = f; f = e; e = d + temp1;
      d = c; c = b; b = a; a = temp1 + temp2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  std::string out;
  out.reserve(32);
  for (auto x : h) {
    for (int i = 3; i >= 0; --i) {
      out.push_back(static_cast<char>((x >> (i * 8)) & 0xFF));
    }
  }
  return out;
}

// HMAC-SHA256（RFC 2104）：内部实现，供 hmacSha256Raw / PBKDF2 复用
std::string hmacSha256RawImpl(const std::string& key, const std::string& data) {
  std::string k = key;
  if (k.size() > 64) k = sha256Raw(k);
  if (k.size() < 64) k.append(64 - k.size(), '\0');

  std::string ipad(64, '\0'), opad(64, '\0');
  for (int i = 0; i < 64; ++i) {
    uint8_t kk = static_cast<uint8_t>(k[i]);
    ipad[i] = static_cast<char>(kk ^ 0x36);
    opad[i] = static_cast<char>(kk ^ 0x5c);
  }
  return sha256Raw(opad + sha256Raw(ipad + data));
}

} // namespace

std::string sha256Hex(const std::string& input) {
  static const char* kHex = "0123456789abcdef";
  std::string raw = sha256Raw(input);
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0F]);
  }
  return out;
}

std::string hmacSha256Raw(const std::string& key, const std::string& data) {
  return hmacSha256RawImpl(key, data);
}

std::string pbkdf2HmacSha256(const std::string& password, const std::string& salt,
                             unsigned iterations, size_t dkLen) {
  // RFC 2898 §5.2：DK = T1 || T2 || ... ; Ti = F(P, S, c, i)，F = U1 ^ U2 ^ ... ^ Uc
  size_t blocks = (dkLen + 31) / 32; // 每块 32 字节（SHA-256）
  std::string out;
  out.reserve(blocks * 32);
  for (size_t i = 1; i <= blocks; ++i) {
    std::string blockSalt = salt;
    blockSalt.push_back(static_cast<char>((i >> 24) & 0xFF));
    blockSalt.push_back(static_cast<char>((i >> 16) & 0xFF));
    blockSalt.push_back(static_cast<char>((i >> 8) & 0xFF));
    blockSalt.push_back(static_cast<char>(i & 0xFF));
    std::string u = hmacSha256RawImpl(password, blockSalt);
    std::string t = u;
    for (unsigned c = 1; c < iterations; ++c) {
      u = hmacSha256RawImpl(password, u);
      for (size_t j = 0; j < t.size(); ++j) {
        t[j] = static_cast<char>(static_cast<uint8_t>(t[j]) ^ static_cast<uint8_t>(u[j]));
      }
    }
    out += t;
  }
  out.resize(dkLen);
  return out;
}

std::string pbkdf2HmacSha256Hex(const std::string& password, const std::string& salt,
                                unsigned iterations, size_t dkLen) {
  std::string raw = pbkdf2HmacSha256(password, salt, iterations, dkLen);
  std::ostringstream oss;
  for (unsigned char c : raw) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(c);
  }
  return oss.str();
}

} // namespace im