#pragma once

#include "rate_limiter_store.hpp"
#include "token_store.hpp"
#include "redis_client.hpp"

#include <chrono>
#include <cstdint>

namespace im {

// Redis 滑动窗口限流：ZSET 记录窗口内调用时间戳（score=时间戳毫秒），
// 先剔除窗口外记录再计数。跨实例共享、重启后计数保留。
class RedisRateLimiter : public RateLimiterStore {
public:
  explicit RedisRateLimiter(const RedisStoreConfig& cfg);
  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  bool allow(int64_t userId, size_t maxCalls, std::chrono::milliseconds window) override;
  void reset(int64_t userId) override;

private:
  RedisClient client_;
};

} // namespace im