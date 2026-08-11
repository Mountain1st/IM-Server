#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace im {

namespace ws {

constexpr char kMagicGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

enum class Opcode : uint8_t {
  Continuation = 0x0,
  Text         = 0x1,
  Binary       = 0x2,
  Close        = 0x8,
  Ping         = 0x9,
  Pong         = 0xA,
};

// 从握手请求头解析出 Sec-WebSocket-Key
bool parseHandshakeKey(const std::string& request, std::string* key);

// 校验握手请求头（RFC 6455）：
// - 首行必须为 GET
// - 必须包含 Upgrade: websocket、Connection: Upgrade
// - 必须包含 Sec-WebSocket-Key（通过 key 参数带回）
// - 版本必须为 13
// 合法返回 true；非法返回 false 并附 reason
bool validateHandshake(const std::string& request, std::string* key, std::string* reason);

// 生成 Sec-WebSocket-Accept
std::string computeAccept(const std::string& key);

// 解析 WebSocket 帧（单帧）。返回：
//   ok=true  解析出一个完整帧
//   ok=false needMore=true   数据不足，等待更多
//   ok=false needMore=false  协议错误
struct Frame {
  bool fin = true;
  Opcode opcode = Opcode::Text;
  std::string payload;
};
struct ParseResult {
  bool ok = false;
  bool needMore = false;
};
// 帧解析器：支持跨包累积，暂未处理分片重组（分片由上层完成）
class FrameParser {
public:
  void feed(const char* data, size_t len);
  bool parseOne(Frame* frame); // 有完整帧返回 true
  // 限制累积缓冲上限，防恶意大帧/洪泛撑爆内存；0 表示不限制
  void setMaxBytes(size_t maxBytes) { maxBytes_ = maxBytes; }
  bool overflow() const { return overflow_; }
private:
  // 已解析的字节游标：避免每帧对 buf_ 整体 erase(0,n) 的 O(n) 前移
  void compact();

  std::string buf_;
  size_t pos_ = 0; // buf_ 中待解析数据的起始位置
  size_t maxBytes_ = 0;
  bool overflow_ = false;
};

// 构造一个服务端发送帧（服务端不发掩码）
std::string buildFrame(Opcode opcode, const std::string& payload, bool fin = true);

} // namespace ws

} // namespace im