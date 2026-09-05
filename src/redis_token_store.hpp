#pragma once

#include "token_store.hpp"

#include <hiredis/hiredis.h>

#include <mutex>
#include <string>
#include <vector>

namespace im {

// hiredis 的 RAII 封装：连接生命周期、命令执行与断线重建。
// 单个 redisContext 非线程安全，内部用互斥锁串行化。
class RedisClient {
public:
  RedisClient(const std::string& host, unsigned port,
              const std::string& password, int db);
  ~RedisClient();

  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  bool ok() const { return ok_; }
  std::string errStr() const { return errStr_; }

  // 执行命令并返回整数 reply（如 SETEX 的 OK/1、DEL 的 1、GET 的 userId）。
  // 通过 replyKind 区分：GET 返回字符串 / 整数 / 空。
  // 返回 -2 表示连接/命令失败（调用方决定重连或报错）。
  int64_t commandInt(const std::vector<std::string>& args);
  // 执行返回字符串的命令（GET 等）。成功返回 true 并写出 value（不存在则 value 为空）。
  bool commandString(const std::vector<std::string>& args, std::string* value);

private:
  void connect();
  redisContext* ctx_ = nullptr;
  std::string host_;
  unsigned port_;
  std::string password_;
  int db_;
  bool ok_ = false;
  std::string errStr_;
  std::mutex mutex_; // 串行化连接访问
};

// Redis token 存储：SETEX digest ttl userId / GET / DEL。
class RedisTokenStore : public TokenStore {
public:
  explicit RedisTokenStore(const RedisStoreConfig& cfg);
  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  void put(const std::string& tokenDigest, int64_t userId,
           std::chrono::seconds ttl) override;
  int get(const std::string& tokenDigest, int64_t* userId) override;
  void remove(const std::string& tokenDigest) override;

private:
  RedisClient client_;
};

} // namespace im