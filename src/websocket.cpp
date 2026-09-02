#include "websocket.hpp"

#include "util.hpp"

#include <arpa/inet.h>
#include <endian.h>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <sstream>

namespace im {
namespace ws {

bool parseHandshakeKey(const std::string& request, std::string* key) {
  std::istringstream stream(request);
  std::string line;
  while (std::getline(stream, line)) {
    // 去掉可能的 \r
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("Sec-WebSocket-Key:", 0) == 0) {
      std::string value = line.substr(18);
      // 去除首尾空格
      size_t start = value.find_first_not_of(" \t");
      size_t end = value.find_last_not_of(" \t");
      if (start == std::string::npos) return false;
      *key = value.substr(start, end - start + 1);
      return !key->empty();
    }
  }
  return false;
}

namespace {
// 大小写不敏感比较两个字符串
bool iequals(const std::string& a, const char* b) {
  size_t len = std::strlen(b);
  if (a.size() != len) return false;
  for (size_t i = 0; i < len; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// 从请求头解析某个字段的值（字段名大小写不敏感匹配）
bool getHeader(const std::string& request, const char* name, std::string* value) {
  size_t nameLen = std::strlen(name);
  std::istringstream stream(request);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > nameLen && line[nameLen] == ':' &&
        iequals(line.substr(0, nameLen), name)) {
      std::string v = line.substr(nameLen + 1);
      size_t start = v.find_first_not_of(" \t");
      size_t end = v.find_last_not_of(" \t");
      if (start == std::string::npos) {
        value->clear();
      } else {
        *value = v.substr(start, end - start + 1);
      }
      return true;
    }
  }
  return false;
}

// Connection 值可能为 "keep-alive, Upgrade" 等多 token，逐个大小写不敏感匹配
bool headerHasToken(const std::string& value, const char* token) {
  std::istringstream stream(value);
  std::string part;
  while (std::getline(stream, part, ',')) {
    size_t start = part.find_first_not_of(" \t");
    size_t end = part.find_last_not_of(" \t");
    if (start == std::string::npos) continue;
    if (iequals(part.substr(start, end - start + 1), token)) return true;
  }
  return false;
}

// 从 URL/Origin 中提取 host[:port]（去掉 scheme:// 前缀与路径），并统一转小写
std::string extractHostLower(const std::string& s) {
  std::string t = s;
  size_t scheme = t.find("://");
  if (scheme != std::string::npos) t = t.substr(scheme + 3);
  size_t p = t.find_first_of("/?#");
  if (p != std::string::npos) t = t.substr(0, p);
  for (auto& c : t) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return t;
}
} // namespace

std::string getRequestHeader(const std::string& request, const char* name) {
  std::string value;
  if (getHeader(request, name, &value)) return value;
  return {};
}

bool originAllowed(const std::string& origin, const std::vector<std::string>& allow) {
  std::string originHost = extractHostLower(origin);
  if (originHost.empty()) return false;
  for (const auto& a : allow) {
    if (extractHostLower(a) == originHost) return true;
  }
  return false;
}

bool hostAllowed(const std::string& host, const std::vector<std::string>& allow) {
  std::string h = extractHostLower(host);
  if (h.empty()) return false;
  for (const auto& a : allow) {
    if (extractHostLower(a) == h) return true;
  }
  return false;
}

bool validateHandshake(const std::string& request, std::string* key, std::string* reason) {
  // 首行必须是 GET
  if (request.rfind("GET ", 0) != 0) {
    *reason = "method must be GET";
    return false;
  }
  // Upgrade 头必须含 websocket
  std::string upgrade;
  if (!getHeader(request, "Upgrade", &upgrade) || !iequals(upgrade, "websocket")) {
    *reason = "missing Upgrade: websocket";
    return false;
  }
  // Connection 头必须含 Upgrade（可能为 "keep-alive, Upgrade" 等）
  std::string connection;
  if (!getHeader(request, "Connection", &connection) || !headerHasToken(connection, "Upgrade")) {
    *reason = "missing Connection: Upgrade";
    return false;
  }
  // 版本必须为 13
  std::string version;
  if (getHeader(request, "Sec-WebSocket-Version", &version)) {
    if (version != "13") {
      *reason = "unsupported websocket version";
      return false;
    }
  } else {
    *reason = "missing Sec-WebSocket-Version";
    return false;
  }
  // 必须携带 Sec-WebSocket-Key
  if (!parseHandshakeKey(request, key)) {
    *reason = "missing Sec-WebSocket-Key";
    return false;
  }
  return true;
}

std::string computeAccept(const std::string& key) {
  std::string digest = sha1Raw(key + kMagicGuid);
  return base64Encode(digest);
}

void FrameParser::feed(const char* data, size_t len) {
  if (overflow_) return;
  // 只统计未解析部分的长度，已解析字节(pos_)不计入累积上限
  if (maxBytes_ > 0 && (buf_.size() - pos_) + len > maxBytes_) {
    overflow_ = true;
    return;
  }
  buf_.append(data, len);
}

// 游标过大时一次性前移，摊还 O(1)；避免每帧整体 erase(0,n) 的 O(n²) 退化
void FrameParser::compact() {
  if (pos_ == 0) return;
  if (pos_ >= 4096 || pos_ * 2 >= buf_.size()) {
    buf_.erase(0, pos_);
    pos_ = 0;
  }
}

bool FrameParser::parseOne(Frame* frame) {
  if (buf_.size() - pos_ < 2) return false; // need more

  uint8_t b0 = static_cast<uint8_t>(buf_[pos_]);
  uint8_t b1 = static_cast<uint8_t>(buf_[pos_ + 1]);
  bool fin = (b0 & 0x80) != 0;
  uint8_t opcode = b0 & 0x0F;
  bool masked = (b1 & 0x80) != 0;
  uint64_t len = b1 & 0x7F;

  size_t offset = pos_ + 2;
  if (len == 126) {
    if (buf_.size() < offset + 2) return false;
    uint16_t v;
    std::memcpy(&v, buf_.data() + offset, 2);
    len = ntohs(v);
    offset += 2;
  } else if (len == 127) {
    if (buf_.size() < offset + 8) return false;
    uint64_t v;
    std::memcpy(&v, buf_.data() + offset, 8);
    len = be64toh(v);
    offset += 8;
  }

  if (len > (64u << 20)) { // 单帧上限 64MB，防御恶意大帧
    return false; // 协议错误
  }

  char mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (buf_.size() < offset + 4) return false;
    std::memcpy(mask, buf_.data() + offset, 4);
    offset += 4;
  }

  if (buf_.size() < offset + len) return false; // need more payload

  frame->fin = fin;
  frame->opcode = static_cast<Opcode>(opcode);
  frame->payload.assign(buf_.data() + offset, static_cast<size_t>(len));

  if (masked) {
    for (size_t i = 0; i < frame->payload.size(); ++i) {
      frame->payload[i] ^= mask[i % 4];
    }
  }

  pos_ = offset + static_cast<size_t>(len);
  compact();
  return true;
}

std::string buildFrame(Opcode opcode, const std::string& payload, bool fin) {
  std::string out;
  uint8_t b0 = (fin ? 0x80 : 0) | static_cast<uint8_t>(opcode);
  out.push_back(static_cast<char>(b0));

  size_t len = payload.size();
  if (len < 126) {
    out.push_back(static_cast<char>(len));
  } else if (len <= 0xFFFF) {
    out.push_back(static_cast<char>(126));
    uint16_t v = htons(static_cast<uint16_t>(len));
    out.append(reinterpret_cast<const char*>(&v), 2);
  } else {
    out.push_back(static_cast<char>(127));
    uint64_t v = htobe64(static_cast<uint64_t>(len));
    out.append(reinterpret_cast<const char*>(&v), 8);
  }
  out += payload;
  return out;
}

} // namespace ws
} // namespace im