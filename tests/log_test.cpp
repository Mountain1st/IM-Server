#include "test_util.hpp"

#include "log.hpp"

#include <cstdio>
#include <string>
#include <unistd.h>

using namespace im;

TEST(log_level_filter) {
  log::Log::setLevel(log::Level::WARN);
  FILE* f = tmpfile();
  log::Log::setFile(f);
  log::Log::write(log::Level::INFO, "t.cpp", 1, "hidden-msg");
  log::Log::write(log::Level::ERROR, "t.cpp", 2, "shown-msg");
  std::fflush(f);
  std::rewind(f);
  char buf[1024] = {0};
  size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::string s(buf, n);
  CHECK(s.find("hidden-msg") == std::string::npos);
  CHECK(s.find("shown-msg") != std::string::npos);
  log::Log::shutdown();
  log::Log::setLevel(log::Level::INFO);
}

TEST(log_format_fields) {
  FILE* f = tmpfile();
  log::Log::setFile(f);
  log::Log::write(log::Level::WARN, "myfile.cpp", 42, "hello");
  std::fflush(f);
  std::rewind(f);
  char buf[1024] = {0};
  size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::string s(buf, n);
  CHECK(s.find("WARN") != std::string::npos);
  CHECK(s.find("myfile.cpp:42") != std::string::npos);
  CHECK(s.find("hello") != std::string::npos);
  log::Log::shutdown();
}

TEST(log_macros_emit) {
  FILE* f = tmpfile();
  log::Log::setFile(f);
  LOG_I("num=" << 7);
  LOG_E(std::string("oops"));
  std::fflush(f);
  std::rewind(f);
  char buf[1024] = {0};
  size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::string s(buf, n);
  CHECK(s.find("num=7") != std::string::npos);
  CHECK(s.find("oops") != std::string::npos);
  log::Log::shutdown();
}


// 按大小轮转：写入超过 IM_LOG_ROTATE_BYTES 阈值后应生成 .1 备份并继续写主文件
TEST(log_rotation_by_size) {
  const std::string base = "/tmp/ims_log_rot_test.log";
  std::remove(base.c_str());
  std::remove((base + ".1").c_str());
  // 经 init 路径配置文件与轮转参数（环境变量）
  setenv("IM_LOG_FILE", base.c_str(), 1);
  setenv("IM_LOG_ROTATE_BYTES", "200", 1);  // 200 字节即轮转（测试用小阈值）
  setenv("IM_LOG_KEEP", "2", 1);
  log::Log::init();
  // 写超过阈值的日志触发轮转
  for (int i = 0; i < 20; ++i) {
    log::Log::write(log::Level::ERROR, "t.cpp", i, "rotation-probe-line-padding-xxxx");
  }
  log::Log::shutdown();
  // 断言：备份文件存在且非空；主文件存在（继续写）
  FILE* b1 = std::fopen((base + ".1").c_str(), "r");
  CHECK(b1 != nullptr);
  if (b1) {
    std::fseek(b1, 0, SEEK_END);
    CHECK(std::ftell(b1) > 0);
    std::fclose(b1);
  }
  FILE* b0 = std::fopen(base.c_str(), "r");
  CHECK(b0 != nullptr);
  if (b0) {
    std::fclose(b0);
  }
  std::remove(base.c_str());
  std::remove((base + ".1").c_str());
  std::remove((base + ".2").c_str());
  unsetenv("IM_LOG_FILE");
  unsetenv("IM_LOG_ROTATE_BYTES");
  unsetenv("IM_LOG_KEEP");
}

int main() {
  RUN(log_level_filter);
  RUN(log_format_fields);
  RUN(log_macros_emit);
  RUN(log_rotation_by_size);
  std::printf("log_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
