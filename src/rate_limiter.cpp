#include "rate_limiter.hpp"

namespace im {

bool RateLimiter::allow(int64_t userId, size_t maxCalls, std::chrono::milliseconds window) {
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

void RateLimiter::reset(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  calls_.erase(userId);
}

} // namespace im
