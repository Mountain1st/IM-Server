// ============================================================
// RateLimiter（内存后端）：滑动窗口限流（允许 N 次/窗口）
// zset 语义的时间戳队列实现；allow() 内部淘汰过期条目。
// 发送限流与查询限流共用（Redis 后端键前缀区分：rate:/qrate:）
// ============================================================
#include "rate_limiter_store.hpp"

#include <iostream>

namespace im {

bool MemoryRateLimiter::allow(int64_t userId, size_t maxCalls,
                              std::chrono::milliseconds window) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  auto& q = calls_[userId];
  // 剔除窗口外的调用记录
  while (!q.empty() && now - q.front() >= window) q.pop_front();
  if (q.size() >= maxCalls) return false;
  q.push_back(now);
  // 防表无限膨胀：超过阈值时清理已无记录的（空队列）用户项
  if (calls_.size() > 4096) {
    for (auto it = calls_.begin(); it != calls_.end();) {
      if (it->second.empty()) it = calls_.erase(it);
      else ++it;
    }
  }
  return true;
}

void MemoryRateLimiter::reset(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  calls_.erase(userId);
}

} // namespace im