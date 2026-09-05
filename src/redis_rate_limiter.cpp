#include "redis_rate_limiter.hpp"

namespace im {

RedisRateLimiter::RedisRateLimiter(const RedisStoreConfig& cfg)
  : client_(cfg.redisHost, cfg.redisPort, cfg.redisPassword, cfg.redisDb) {}

bool RedisRateLimiter::allow(int64_t userId, size_t maxCalls,
                             std::chrono::milliseconds window) {
  if (maxCalls == 0) return false;
  const std::string key = "rate:" + std::to_string(userId);
  const int64_t now = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
  const int64_t cutoff = now - window.count();
  // 1) 剔除窗口外记录
  client_.commandInt({"ZREMRANGEBYSCORE", key, "-inf", std::to_string(cutoff)});
  // 2) 计数
  const int64_t count = client_.commandInt({"ZCARD", key});
  if (count < 0) return false; // Redis 不可用：按拒绝处理（fail-closed）
  if (static_cast<size_t>(count) >= maxCalls) return false;
  // 3) 记录本次调用（member 用 时间戳-随机 保证唯一），并延长 key 存活
  const std::string member = std::to_string(now) + "-" + std::to_string(count);
  client_.commandInt({"ZADD", key, std::to_string(now), member});
  client_.commandInt({"EXPIRE", key, std::to_string(window.count() / 1000 + 1)});
  return true;
}

void RedisRateLimiter::reset(int64_t userId) {
  client_.commandInt({"DEL", "rate:" + std::to_string(userId)});
}

} // namespace im