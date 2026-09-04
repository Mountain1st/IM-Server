#include "websocket.hpp"

#include "server_config.hpp"
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
// 校验 Sec-WebSocket-Key 格式：必须是 16 字节随机数的 Base64（22 字符 + "=="）
bool validKeyFormat(const std::string& k) {
  auto isB64 = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
  };
  if (k.size() != 24) return false;
  for (size_t i = 0; i < 22; ++i) {
    if (!isB64(k[i])) return false;
  }
  return k[22] == '=' && k[23] == '=';
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
  // 请求行必须为 "GET / HTTP/1.1"（协议文档约定路径为 /）
  size_t lineEnd = request.find("\r\n");
  std::string line0 = lineEnd == std::string::npos ? request : request.substr(0, lineEnd);
  std::istringstream ls(line0);
  std::string method, path, version;
  ls >> method >> path >> version;
  if (method != "GET") {
    *reason = "method must be GET";
    return false;
  }
  if (path != "/") {
    *reason = "invalid request path";
    return false;
  }
  if (version != "HTTP/1.1") {
    *reason = "must be HTTP/1.1";
    return false;
  }
  // HTTP/1.1 必须携带 Host 头（RFC 7230 §5.4）
  std::string host;
  if (!getHeader(request, "Host", &host) || host.empty()) {
    *reason = "missing Host header";
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
  std::string versionHeader;
  if (getHeader(request, "Sec-WebSocket-Version", &versionHeader)) {
    if (versionHeader != "13") {
      *reason = "unsupported websocket version";
      return false;
    }
  } else {
    *reason = "missing Sec-WebSocket-Version";
    return false;
  }
  // 必须携带合法格式的 Sec-WebSocket-Key（大小写不敏感取头）
  if (!getHeader(request, "Sec-WebSocket-Key", key) || !validKeyFormat(*key)) {
    *reason = "missing or invalid Sec-WebSocket-Key";
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

ParseResult FrameParser::parseOne(Frame* frame) {
  if (buf_.size() - pos_ < 2) return ParseResult::NeedMore;

  uint8_t b0 = static_cast<uint8_t>(buf_[pos_]);
  uint8_t b1 = static_cast<uint8_t>(buf_[pos_ + 1]);
  bool fin = (b0 & 0x80) != 0;
  // RSV1/RSV2/RSV3：本项目未协商任何扩展，必须为 0
  if ((b0 & 0x70) != 0) return ParseResult::ProtocolError;
  uint8_t opcode = b0 & 0x0F;
  bool masked = (b1 & 0x80) != 0;
  uint64_t len = b1 & 0x7F;

  // opcode 合法性：数据帧（0/1/2）与控制帧（8/9/A）
  switch (opcode) {
  case 0x0: case 0x1: case 0x2: case 0x8: case 0x9: case 0xA:
    break;
  default:
    return ParseResult::ProtocolError; // 非法 opcode
  }
  // 控制帧（Close/Ping/Pong）：必须 FIN 且 payload ≤125（RFC 6455 §5.5）
  if (opcode >= 0x8) {
    if (!fin) return ParseResult::ProtocolError;
    if (len > 125) return ParseResult::ProtocolError;
  }
  // 客户端帧必须掩码（RFC 6455 §5.1：服务器必须拒绝未掩码的客户端帧，
  // 防缓存投毒）
  if (!masked) return ParseResult::ProtocolError;

  size_t offset = pos_ + 2;
  if (len == 126) {
    if (buf_.size() < offset + 2) return ParseResult::NeedMore;
    uint16_t v;
    std::memcpy(&v, buf_.data() + offset, 2);
    len = ntohs(v);
    // 最小长度编码：7 位码为 126 时实际长度必须 ≥126
    if (len < 126) return ParseResult::ProtocolError;
    offset += 2;
  } else if (len == 127) {
    if (buf_.size() < offset + 8) return ParseResult::NeedMore;
    uint64_t v;
    std::memcpy(&v, buf_.data() + offset, 8);
    len = be64toh(v);
    // RFC 6455 §5.2：64 位长度最高位必须为 0，且最小长度编码（≥65536）
    if ((v & 0x8000000000000000ULL) != 0) return ParseResult::ProtocolError;
    if (len < 65536) return ParseResult::ProtocolError;
    offset += 8;
  }

  if (len > config::kMaxFrameBytes) return ParseResult::ProtocolError; // 单帧上限

  char mask[4] = {0, 0, 0, 0};
  if (buf_.size() < offset + 4) return ParseResult::NeedMore;
  std::memcpy(mask, buf_.data() + offset, 4);
  offset += 4;

  if (buf_.size() < offset + len) return ParseResult::NeedMore;

  frame->fin = fin;
  frame->opcode = static_cast<Opcode>(opcode);
  frame->masked = masked;
  frame->payload.assign(buf_.data() + offset, static_cast<size_t>(len));
  for (size_t i = 0; i < frame->payload.size(); ++i) {
    frame->payload[i] ^= mask[i % 4];
  }

  // Close 帧 payload：0 字节合法；1 字节违反 RFC（code 必须 2 字节）；
  // ≥2 字节时前 2 字节为原始 Close code（16 位整数，非 UTF-8），
  // 后续字节为 UTF-8 reason 字符串（RFC 6455 §5.5.1/§7.4）。
  if (opcode == 0x8) {
    if (frame->payload.size() == 1) return ParseResult::ProtocolError;
    if (frame->payload.size() >= 2) {
      uint16_t code = (static_cast<uint8_t>(frame->payload[0]) << 8) |
                      static_cast<uint8_t>(frame->payload[1]);
      if (!isValidCloseCode(code)) return ParseResult::ProtocolError;
      // 只校验 reason 部分；前 2 字节是原始二进制 code，不能按 UTF-8 校验
      if (!isValidUtf8(frame->payload.substr(2))) return ParseResult::ProtocolError;
    }
  }

  pos_ = offset + static_cast<size_t>(len);
  compact();
  return ParseResult::Complete;
}

bool isValidUtf8(const std::string& s) {
  // RFC 3629 校验：拒绝过短/过长编码、续字节错位、代理区与越界码点。
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    uint8_t c = static_cast<uint8_t>(s[i]);
    if (c < 0x80) { ++i; continue; }
    size_t need; uint32_t cp;
    if ((c & 0xE0) == 0xC0) { need = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 4; cp = c & 0x07; }
    else return false; // 单独续字节或非法首字节
    if (i + need > n) return false;
    for (size_t k = 1; k < need; ++k) {
      uint8_t cc = static_cast<uint8_t>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    // 拒绝过短编码（overlong）与越界/代理区
    if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) ||
        (need == 4 && cp < 0x10000)) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    i += need;
  }
  return true;
}

bool isValidCloseCode(uint16_t code) {
  // RFC 6455 §7.4.1/§7.4.2：保留码段 0-999、1004-1006、1012-2999 不允许
  if (code >= 1000 && code <= 1003) return true;
  if (code >= 1007 && code <= 1011) return true;
  if (code >= 3000 && code <= 4999) return true;
  return false;
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