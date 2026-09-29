// ThreadPool 停机排水单元测试：stop 的 drain 语义与超时保护
// 1) drain：stop 后队列中已入队任务必须全部执行完才退出
// 2) 超时：慢任务拖住排水时，stop(短超时) 不挂死，放弃剩余任务退出

#include "threadpool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using im::ThreadPool;

static int g_fail = 0;

#define CHECK(expr, msg)                     \
  do {                                       \
    if (expr) {                              \
      std::printf("PASS %s\n", msg);         \
    } else {                                 \
      std::printf("FAIL %s\n", msg);         \
      ++g_fail;                              \
    }                                        \
  } while (0)

int main() {
  // ---- 用例 1：drain 语义——stop 后队列清空才退 ----
  {
    ThreadPool pool(2);
    pool.start();
    std::atomic<int> done{0};
    // 50 个快任务压入队列后立即停止：全部须被执行
    for (int i = 0; i < 50; ++i) {
      pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.stop();
    CHECK(done.load() == 50, "drain executes all queued tasks");
  }

  // ---- 用例 2：超时保护——慢任务排水超时后不挂死 ----
  {
    ThreadPool pool(1);
    pool.start();
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    // 10 个 120ms 慢任务，总时长约 1.2s；stop 只给 300ms 排水窗口
    for (int i = 0; i < 10; ++i) {
      pool.submit([&] {
        started.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        finished.fetch_add(1, std::memory_order_relaxed);
      });
    }
    const auto t0 = std::chrono::steady_clock::now();
    pool.stop(300);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    // 不挂死：stop 必然返回；手头任务做完即退（120ms 级），
    // 剩余任务被放弃（started < 10），执行过的必然完整结束（中途不弃单任务）
    CHECK(elapsed.count() < 1500, "stop with timeout returns promptly");
    CHECK(started.load() < 10, "abandon drops remaining tasks");
    CHECK(finished.load() == started.load(), "in-flight task completes fully");
  }

  // ---- 用例 3：空池停止（无任务） ----
  {
    ThreadPool pool(2);
    pool.start();
    pool.stop();
    CHECK(true, "empty pool stops cleanly");
  }

  std::printf("%s: %d failures\n", g_fail ? "FAILED" : "ALL OK", g_fail);
  return g_fail ? 1 : 0;
}
