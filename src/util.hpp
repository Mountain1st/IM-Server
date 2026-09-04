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
// SHA-256 摘要，输出 64 字符 hex（用于 token 等敏感数据的摘要化存储）
std::string sha256Hex(const std::string& input);
// Base64 编码
std::string base64Encode(const std::string& input);

// HMAC-SHA256 摘要，输出 32 字节（二进制）
std::string hmacSha256Raw(const std::string& key, const std::string& data);
// PBKDF2-HMAC-SHA256（RFC 2898）：迭代 iterations 次，输出 dkLen 字节（二进制）。
// 密码派生用慢哈希，抵御 GPU/ASIC 暴力破解。
std::string pbkdf2HmacSha256(const std::string& password, const std::string& salt,
                             unsigned iterations, size_t dkLen);
// PBKDF2-HMAC-SHA256 的 hex 封装（输出 2*dkLen 字符）
std::string pbkdf2HmacSha256Hex(const std::string& password, const std::string& salt,
                                unsigned iterations, size_t dkLen);

} // namespace im