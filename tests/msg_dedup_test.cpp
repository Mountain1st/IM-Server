#include "test_util.hpp"

#include "msg_dedup.hpp"

#include <string>
#include <thread>
#include <vector>

using namespace im;

TEST(dedup_is_duplicate_false_on_first) {
  MemoryMsgDedup d;
  CHECK(!d.isDuplicate(1, "id1"));
}

TEST(dedup_remember_then_duplicate) {
  MemoryMsgDedup d;
  d.remember(1, "m1");
  CHECK(d.isDuplicate(1, "m1"));
  // 不同用户互不影响
  CHECK(!d.isDuplicate(2, "m1"));
  // 不同 id 不影响
  CHECK(!d.isDuplicate(1, "m2"));
}

TEST(dedup_empty_id_noop) {
  MemoryMsgDedup d;
  d.remember(1, "");
  CHECK(!d.isDuplicate(1, ""));
}

TEST(dedup_order_semantics) {
  // 关键：isDuplicate 只查不记（P0-6 修复依赖此语义）
  MemoryMsgDedup d;
  CHECK(!d.isDuplicate(1, "x"));
  CHECK(!d.isDuplicate(1, "x")); // 未 remember 前重复查询仍为 false
  d.remember(1, "x");
  CHECK(d.isDuplicate(1, "x"));
}

TEST(dedup_concurrent_remember) {
  MemoryMsgDedup d;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&d, i] {
      for (int j = 0; j < 1000; ++j) {
        d.remember(i, "id" + std::to_string(j));
      }
    });
  }
  for (auto& t : threads) t.join();
  for (int i = 0; i < 8; ++i) {
    CHECK(d.isDuplicate(i, "id999"));
  }
}

int main() {
  RUN(dedup_is_duplicate_false_on_first);
  RUN(dedup_remember_then_duplicate);
  RUN(dedup_empty_id_noop);
  RUN(dedup_order_semantics);
  RUN(dedup_concurrent_remember);
  std::printf("msg_dedup_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}