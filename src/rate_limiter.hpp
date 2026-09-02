#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace im {

// 每用户滑动窗口限流（内存版）：窗口时长内，用户调用次数超过上限则拒绝。
// 多线程访问，内部互斥锁保护；allow() 时顺带清理空条目，避免表无限膨胀。
class RateLimiter {
public:
  // 在窗口 window 内允许 maxCalls 次调用；返回 true 表示允许本次调用。
  bool allow(int64_t userId, size_t maxCalls, std::chrono::milliseconds window);
  // 清除某用户全部记录（登出时可选调用，释放内存）。
  void reset(int64_t userId);

private:
  std::mutex mutex_;
  // userId -> 窗口内的调用时间戳（升序，队首最旧）
  std::unordered_map<int64_t, std::deque<std::chrono::steady_clock::time_point>> calls_;
};

} // namespace im
