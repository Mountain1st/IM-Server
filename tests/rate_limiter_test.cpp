#include "test_util.hpp"

#include "rate_limiter_store.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace im;

TEST(limiter_allows_under_limit) {
  MemoryRateLimiter r;
  for (int i = 0; i < 3; ++i) {
    CHECK(r.allow(1, 3, std::chrono::milliseconds(1000)));
  }
}

TEST(limiter_rejects_over_limit) {
  MemoryRateLimiter r;
  for (int i = 0; i < 3; ++i) r.allow(1, 3, std::chrono::milliseconds(1000));
  CHECK(!r.allow(1, 3, std::chrono::milliseconds(1000)));
}

TEST(limiter_per_user_independent) {
  MemoryRateLimiter r;
  for (int i = 0; i < 3; ++i) r.allow(1, 3, std::chrono::milliseconds(1000));
  CHECK(!r.allow(1, 3, std::chrono::milliseconds(1000)));
  CHECK(r.allow(2, 3, std::chrono::milliseconds(1000))); // 用户 2 不受影响
}

TEST(limiter_window_expires) {
  MemoryRateLimiter r;
  for (int i = 0; i < 2; ++i) r.allow(1, 2, std::chrono::milliseconds(50));
  CHECK(!r.allow(1, 2, std::chrono::milliseconds(50)));
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  CHECK(r.allow(1, 2, std::chrono::milliseconds(50))); // 窗口过期恢复
}

TEST(limiter_reset) {
  MemoryRateLimiter r;
  for (int i = 0; i < 2; ++i) r.allow(1, 2, std::chrono::milliseconds(1000));
  CHECK(!r.allow(1, 2, std::chrono::milliseconds(1000)));
  r.reset(1);
  CHECK(r.allow(1, 2, std::chrono::milliseconds(1000)));
}

TEST(limiter_concurrent) {
  MemoryRateLimiter r;
  std::atomic<int> allowed{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&r, &allowed] {
      for (int j = 0; j < 100; ++j) {
        if (r.allow(42, 100, std::chrono::milliseconds(1000))) allowed.fetch_add(1);
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK_EQ(allowed.load(), 100); // 恰好放行 100 次
}

int main() {
  RUN(limiter_allows_under_limit);
  RUN(limiter_rejects_over_limit);
  RUN(limiter_per_user_independent);
  RUN(limiter_window_expires);
  RUN(limiter_reset);
  RUN(limiter_concurrent);
  std::printf("rate_limiter_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}