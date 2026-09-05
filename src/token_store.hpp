#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

namespace im {

// Redis 连接配置。缺省关闭；IM_REDIS_ENABLED=1 时启用（连接失败则启动报错退出）。
// 端口注意：本项目 Redis 容器映射为 6380。
struct RedisStoreConfig {
  bool enabled = false;        // 显式启用才使用 Redis 后端
  std::string redisHost = "127.0.0.1";
  unsigned redisPort = 6380;
  std::string redisPassword;   // 空表示无密码
  int redisDb = 0;
};

// token 存储接口：登录签发的 token 摘要在此存储，供校验/登出。
// 双后端：内存版（MemoryTokenStore，默认）与 Redis 版（RedisTokenStore）。
// 约定：token 以 SHA-256 摘要形式存储（不落明文）；调用方须保证线程安全
// （实现内部自带锁）。
class TokenStore {
public:
  virtual ~TokenStore() = default;

  // 记录 token 摘要 -> userId，ttl 秒后过期。
  virtual void put(const std::string& tokenDigest, int64_t userId,
                   std::chrono::seconds ttl) = 0;
  // 校验 token 摘要。返回 0=有效（写出 userId）；非 0=不存在或已过期。
  virtual int get(const std::string& tokenDigest, int64_t* userId) = 0;
  // 删除 token 摘要（登出）。
  virtual void remove(const std::string& tokenDigest) = 0;
};

// 内存 token 存储（默认）：与旧 UserStore::tokens_ 行为一致，
// 含过期清理与上限淘汰。
class MemoryTokenStore : public TokenStore {
public:
  void put(const std::string& tokenDigest, int64_t userId,
           std::chrono::seconds ttl) override;
  int get(const std::string& tokenDigest, int64_t* userId) override;
  void remove(const std::string& tokenDigest) override;

private:
  struct Entry {
    int64_t userId;
    std::chrono::steady_clock::time_point expireAt;
  };
  void cleanLocked();

  std::mutex mutex_;
  std::unordered_map<std::string, Entry> tokens_;
  // 超过此数时顺带清理过期项，防表无限膨胀
  static constexpr size_t kCleanupThreshold = 1024;
};

} // namespace im