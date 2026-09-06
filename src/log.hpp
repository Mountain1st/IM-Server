#pragma once
// 轻量日志（header-only，自研）：级别 + 时间戳 + 线程 id + 文件行号，线程安全。
// 配置经环境变量：IM_LOG_FILE 为空 = 仅 stderr；IM_LOG_LEVEL 缺省 INFO。
// v1 不做轮转（运维侧截断），后续按需加。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace im {
namespace log {

enum class Level { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Log {
 public:
  // 读 IM_LOG_FILE / IM_LOG_LEVEL 并初始化（main 启动时调用一次）。
  static void init();
  // 单测用：直接设级别 / 输出文件。
  static void setLevel(Level lv);
  static void setFile(FILE* f);
  // 关闭文件输出并回到 stderr（单测清理用）。
  static void shutdown();
  static void write(Level lv, const char* path, int line, const std::string& msg);

 private:
  static const char* levelName(Level lv);
  static std::mutex& mutex();
  static Level& threshold();
  static FILE*& file();
};

inline const char* Log::levelName(Level lv) {
  switch (lv) {
    case Level::DEBUG: return "DEBUG";
    case Level::INFO: return "INFO";
    case Level::WARN: return "WARN";
    case Level::ERROR: return "ERROR";
  }
  return "INFO";
}

inline std::mutex& Log::mutex() {
  static std::mutex m;
  return m;
}

inline Level& Log::threshold() {
  static Level lv = Level::INFO;
  return lv;
}

inline FILE*& Log::file() {
  static FILE* f = stderr;
  return f;
}

inline void Log::setLevel(Level lv) { threshold() = lv; }

inline void Log::setFile(FILE* f) {
  std::lock_guard<std::mutex> lock(mutex());
  if (file() != nullptr && file() != stderr) std::fclose(file());
  file() = (f != nullptr) ? f : stderr;
}

inline void Log::shutdown() { setFile(stderr); }

inline void Log::init() {
  if (const char* v = std::getenv("IM_LOG_LEVEL")) {
    if (std::strcmp(v, "DEBUG") == 0) setLevel(Level::DEBUG);
    else if (std::strcmp(v, "INFO") == 0) setLevel(Level::INFO);
    else if (std::strcmp(v, "WARN") == 0) setLevel(Level::WARN);
    else if (std::strcmp(v, "ERROR") == 0) setLevel(Level::ERROR);
  }
  if (const char* v = std::getenv("IM_LOG_FILE")) {
    if (v[0] != '\0') {
      FILE* f = std::fopen(v, "a");
      if (f != nullptr) setFile(f);
    }
  }
}

inline void Log::write(Level lv, const char* path, int line, const std::string& msg) {
  if (lv < threshold()) return;
  std::time_t t = std::time(nullptr);
  char ts[32] = {0};
  std::tm tmv{};
  localtime_r(&t, &tmv);
  std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
  const char* base = std::strrchr(path, '/');
  base = (base != nullptr) ? base + 1 : path;
  const size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
  std::lock_guard<std::mutex> lock(mutex());
  std::fprintf(file(), "%s [%s] [t%zx] %s:%d %s\n", ts, levelName(lv), tid, base, line,
               msg.c_str());
  std::fflush(file());
}

}  // namespace log
}  // namespace im

// 流式调用：LOG_I("[db] x=" << y); 末尾分号结尾（内部已换行，无需 << std::endl）。
#define LOG_D(...)                                                             \
  do {                                                                         \
    std::ostringstream _log_oss;                                               \
    _log_oss << __VA_ARGS__;                                                   \
    ::im::log::Log::write(::im::log::Level::DEBUG, __FILE__, __LINE__,         \
                          _log_oss.str());                                     \
  } while (0)
#define LOG_I(...)                                                             \
  do {                                                                         \
    std::ostringstream _log_oss;                                               \
    _log_oss << __VA_ARGS__;                                                   \
    ::im::log::Log::write(::im::log::Level::INFO, __FILE__, __LINE__,          \
                          _log_oss.str());                                     \
  } while (0)
#define LOG_W(...)                                                             \
  do {                                                                         \
    std::ostringstream _log_oss;                                               \
    _log_oss << __VA_ARGS__;                                                   \
    ::im::log::Log::write(::im::log::Level::WARN, __FILE__, __LINE__,          \
                          _log_oss.str());                                     \
  } while (0)
#define LOG_E(...)                                                             \
  do {                                                                         \
    std::ostringstream _log_oss;                                               \
    _log_oss << __VA_ARGS__;                                                   \
    ::im::log::Log::write(::im::log::Level::ERROR, __FILE__, __LINE__,         \
                          _log_oss.str());                                     \
  } while (0)
