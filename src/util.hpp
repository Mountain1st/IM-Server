#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace im {

// 设置 fd 为非阻塞
bool setNonBlocking(int fd);
// 禁用 Nagle 算法
bool setNoDelay(int fd);
// 设置 SO_REUSEADDR
bool setReuseAddr(int fd);

// 在调用线程中阻塞 SIGINT/SIGTERM（供工作/IO 线程入口调用），
// 使其绝不会进入信号处理器；成功返回 true
bool blockStopSignals();

// SHA-1 摘要，输出 20 字节（二进制）
std::string sha1Raw(const std::string& input);
// SHA-1 摘要，输出 40 字符 hex
std::string sha1Hex(const std::string& input);
// Base64 编码
std::string base64Encode(const std::string& input);

} // namespace im