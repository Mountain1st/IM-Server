#include "websocket.hpp"

#include "util.hpp"

#include <arpa/inet.h>
#include <endian.h>
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

std::string computeAccept(const std::string& key) {
    std::string digest = sha1Raw(key + kMagicGuid);
    return base64Encode(digest);
}

void FrameParser::feed(const char* data, size_t len) {
    buf_.append(data, len);
}

bool FrameParser::parseOne(Frame* frame) {
    if (buf_.size() < 2) return false; // need more

    uint8_t b0 = static_cast<uint8_t>(buf_[0]);
    uint8_t b1 = static_cast<uint8_t>(buf_[1]);
    bool fin = (b0 & 0x80) != 0;
    uint8_t opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;

    size_t offset = 2;
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

    buf_.erase(0, offset + static_cast<size_t>(len));
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