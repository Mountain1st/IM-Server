#pragma once

#include <cstdio>
#include <cstdlib>

// 极简单元测试宏（无外部依赖）：收集失败，main 返回非 0。
#define TEST(name) \
  static void test_##name()
#define RUN(name)                                          \
  do {                                                     \
    int before = g_failures;                               \
    test_##name();                                         \
    std::printf("  [%s] %s\n", g_failures == before ? "PASS" : "FAIL", #name); \
  } while (0)
#define CHECK(cond)                                         \
  do {                                                      \
    if (!(cond)) {                                          \
      std::printf("    CHECK failed: %s (line %d)\n", #cond, __LINE__); \
      ++g_failures;                                         \
    }                                                       \
  } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

inline int g_failures = 0;