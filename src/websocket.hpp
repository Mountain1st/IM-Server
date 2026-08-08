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
private:
    std::string buf_;
};

// 构造一个服务端发送帧（服务端不发掩码）
std::string buildFrame(Opcode opcode, const std::string& payload, bool fin = true);

} // namespace ws

} // namespace im