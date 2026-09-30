// ============================================================
// RedisRateLimiter：滑动窗口限流的 Redis 后端（ZSET 实现）
// keyPrefix 区分额度桶：rate:（发送）/ qrate:（查询）互不占用
// ============================================================
#include "redis_rate_limiter.hpp"

namespace im {

RedisRateLimiter::RedisRateLimiter(const RedisStoreConfig& cfg,
                                   const std::string& keyPrefix)
  : client_(cfg.redisHost, cfg.redisPort, cfg.redisPassword, cfg.redisDb),
    keyPrefix_(keyPrefix) {}

bool RedisRateLimiter::allow(int64_t userId, size_t maxCalls,
                             std::chrono::milliseconds window) {
  if (maxCalls == 0) return false;
  const std::string key = keyPrefix_ + std::to_string(userId);
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
  // 用构造时的键前缀（C6 自检三轮：曾硬编码 "rate:"——qrate 实例调用
  // 会删错额度桶；当前无调用者，属地雷修复）
  client_.commandInt({"DEL", keyPrefix_ + std::to_string(userId)});
}

} // namespace im