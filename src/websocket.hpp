#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

// 解析请求头中指定字段值（字段名大小写不敏感匹配），未找到返回空串
std::string getRequestHeader(const std::string& request, const char* name);

// 从 X-Forwarded-For 头提取首个客户端 IP（去空格；多段取第一段）。
// 值含非 IP 合法字符（字母换行等，防日志注入）时整体返回空串。
// 仅在"对端属可信代理"时使用（Session 侧判定）。
std::string firstForwardedIp(const std::string& request);

// Origin/Host 校验（防跨站 WebSocket 连接）：
// allow 条目可为 "scheme://host[:port]" 或裸 "host[:port]"，比较时忽略大小写。
// 判断 Origin 头是否被允许；空 origin 恒为 false。
bool originAllowed(const std::string& origin, const std::vector<std::string>& allow);
// 判断 Host 头是否被允许（与 allow 条目的 host[:port] 部分比较，忽略 scheme）。
bool hostAllowed(const std::string& host, const std::vector<std::string>& allow);

// 校验握手请求头（RFC 6455）：
// - 首行必须为 GET
// - 必须包含 Upgrade: websocket、Connection: Upgrade
// - 必须包含 Sec-WebSocket-Key（通过 key 参数带回）
// - 版本必须为 13
// 合法返回 true；非法返回 false 并附 reason
bool validateHandshake(const std::string& request, std::string* key, std::string* reason);

// 生成 Sec-WebSocket-Accept
std::string computeAccept(const std::string& key);

// 解析 WebSocket 帧（单帧）。返回三态结果：
//   NeedMore       数据不足，等待更多（合法）
//   Complete       解析出一个完整帧（frame 有效）
//   ProtocolError  协议错误（RFC 6455 违规，应断开连接）
struct Frame {
  bool fin = true;
  Opcode opcode = Opcode::Text;
  bool masked = false;
  std::string payload;
};
enum class ParseResult {
  NeedMore,
  Complete,
  ProtocolError,
};
// 帧解析器：支持跨包累积，暂未处理分片重组（分片由上层完成）。
// 解析时执行协议校验：RSV 位、opcode 合法性、客户端帧必须掩码、
// 最小长度编码、单帧上限、控制帧 FIN/长度限制、Close payload 的
// UTF-8 与 Close code 合法性。任何违规返回 ProtocolError。
class FrameParser {
public:
  void feed(const char* data, size_t len);
  ParseResult parseOne(Frame* frame);
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

// 校验字符串是否为合法 UTF-8（RFC 3629，含代理区拒绝）。
bool isValidUtf8(const std::string& s);
// 校验 Close code 是否合法（RFC 6455 §7.4；1000-1003、1007-1011、3000-4999）。
bool isValidCloseCode(uint16_t code);

// 构造一个服务端发送帧（服务端不发掩码）
std::string buildFrame(Opcode opcode, const std::string& payload, bool fin = true);

} // namespace ws

} // namespace im