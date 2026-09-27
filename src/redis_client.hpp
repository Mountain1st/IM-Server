#pragma once

#include <hiredis/hiredis.h>

#include <mutex>
#include <string>
#include <vector>

namespace im {

// hiredis 的 RAII 封装：连接生命周期、命令执行与断线重建。
// 单个 redisContext 非线程安全，内部用互斥锁串行化。
// 被 RedisTokenStore / RedisRateLimiter / RedisMsgDedup 等后端共用。
class RedisClient {
public:
  RedisClient(const std::string& host, unsigned port,
              const std::string& password, int db);
  ~RedisClient();

  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  bool ok() const { return ok_; }
  std::string errStr() const { return errStr_; }

  // 执行返回整数 reply 的命令（INCR/DEL/ZCARD/EXPIRE 等）。
  // 返回 -2 表示连接/命令失败（调用方按失败处理）。
  int64_t commandInt(const std::vector<std::string>& args);
  // 执行返回字符串的命令（GET 等）。成功返回 true 并写出 value（不存在则 value 为空）。
  bool commandString(const std::vector<std::string>& args, std::string* value);
  // 执行返回字符串数组的命令（MGET）。成功返回 true 且 out 与请求 key 一一对应
  // （不存在的 key 对应空串）。失败返回 false（调用方按降级处理）。
  bool commandStringArray(const std::vector<std::string>& args,
                          std::vector<std::string>* out);
  // 执行返回整数状态码的布尔命令（SET NX：1=成功 0=已存在，失败=-2）。
  int64_t commandStatus(const std::vector<std::string>& args);

private:
  void connect();
  redisReply* execLocked(const std::vector<std::string>& args);

  redisContext* ctx_ = nullptr;
  std::string host_;
  unsigned port_;
  std::string password_;
  int db_;
  bool ok_ = false;
  std::string errStr_;
  std::mutex mutex_; // 串行化连接访问
};

} // namespace im