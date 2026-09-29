// TcpServer::pickLeastLoadedIndex 单元测试：最少连接优先分配策略
// （替换 fd % N 取模分配，消除 fd 回绕不均与连接寿命差异导致的负载倾斜）

#include "tcpserver.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>

using im::TcpServer;

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

// 构造 n 格原子计数数组（vector<atomic> 只能构造期定长，再逐格 store）
static std::vector<std::atomic<size_t>> makeCounts(
    std::initializer_list<size_t> vals) {
  std::vector<std::atomic<size_t>> counts(vals.size());
  size_t i = 0;
  for (size_t v : vals) counts[i++].store(v, std::memory_order_relaxed);
  return counts;
}

int main() {
  // 空数组：防御性返回 0
  CHECK(TcpServer::pickLeastLoadedIndex(makeCounts({})) == 0, "empty returns 0");

  // 单元素
  CHECK(TcpServer::pickLeastLoadedIndex(makeCounts({9})) == 0, "single returns 0");

  // 基本最小值选择
  CHECK(TcpServer::pickLeastLoadedIndex(makeCounts({3, 1, 2})) == 1,
        "picks min index");

  // 平局：取最靠前的最小者（确定性）
  CHECK(TcpServer::pickLeastLoadedIndex(makeCounts({5, 2, 2, 7})) == 1,
        "tie picks first min");

  // 首位是最小
  CHECK(TcpServer::pickLeastLoadedIndex(makeCounts({0, 4, 4})) == 0,
        "first min wins");

  // 模拟连续 accept：每分配一次计数 +1，下一条选新最小——
  // 结果是严格轮转（0,1,2,0,1,2...），至少与 fd%N 同样均匀
  auto counts = makeCounts({0, 0, 0});
  std::vector<size_t> picked;
  for (int i = 0; i < 9; ++i) {
    const size_t idx = TcpServer::pickLeastLoadedIndex(counts);
    picked.push_back(idx);
    counts[idx].fetch_add(1, std::memory_order_relaxed);
  }
  const std::vector<size_t> expect = {0, 1, 2, 0, 1, 2, 0, 1, 2};
  CHECK(picked == expect, "sequential accepts rotate evenly");

  // 长连接倾斜场景：loop 0 上残留 5 条长连接，新连接应全部避开 0
  auto skewed = makeCounts({5, 0, 0});
  const size_t a = TcpServer::pickLeastLoadedIndex(skewed);
  const size_t b = TcpServer::pickLeastLoadedIndex(skewed);
  CHECK(a == 1 && b == 1, "skewed loop avoided while equal");

  std::printf("%s: %d failures\n", g_fail ? "FAILED" : "ALL OK", g_fail);
  return g_fail ? 1 : 0;
}
