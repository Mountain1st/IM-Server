#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace im {

// 限流存储接口：每用户滑动窗口计数。双后端：
// - MemoryRateLimiter（内存 deque，默认）
// - RedisRateLimiter（ZSET 滑动窗口，跨重启/多实例共享）
// 实现须自身保证线程安全。
class RateLimiterStore {
public:
  virtual ~RateLimiterStore() = default;
  // 在窗口 window 内允许 maxCalls 次调用；返回 true 表示允许本次（并已消耗一次额度）。
  virtual bool allow(int64_t userId, size_t maxCalls,
                     std::chrono::milliseconds window) = 0;
  // 清除某用户全部记录（登出时可选调用）。
  virtual void reset(int64_t userId) = 0;
};

// 内存滑动窗口实现（默认）：userId -> 窗口内调用时间戳队列。
class MemoryRateLimiter : public RateLimiterStore {
public:
  bool allow(int64_t userId, size_t maxCalls, std::chrono::milliseconds window) override;
  void reset(int64_t userId) override;

private:
  std::mutex mutex_;
  std::unordered_map<int64_t,
      std::deque<std::chrono::steady_clock::time_point>> calls_;
};

} // namespace im