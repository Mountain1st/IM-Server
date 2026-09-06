#include "test_util.hpp"

#include "log.hpp"

#include <string>

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

int main() {
  RUN(log_level_filter);
  RUN(log_format_fields);
  RUN(log_macros_emit);
  std::printf("log_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
