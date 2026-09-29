#pragma once
// 轻量日志（header-only，自研）：级别 + 时间戳 + 线程 id + 文件行号，线程安全。
// 配置经环境变量：IM_LOG_FILE 为空 = 仅 stderr；IM_LOG_LEVEL 缺省 INFO。
// 按大小轮转：IM_LOG_ROTATE_BYTES 超过即轮转（缺省 50MB，0 = 不轮转），
// IM_LOG_KEEP 保留备份份数（缺省 5，形如 app.log.1 / .2 / ...）。
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
  static std::string& filePath();      // 轮转用：当前文件路径（空 = 非 init 路径）
  static long& rotateBytes();          // 轮转阈值（0 = 不轮转）
  static int& keepCount();             // 保留备份份数
  static void rotateLocked();          // 假定已持锁时执行轮转
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

inline std::string& Log::filePath() {
  static std::string p;
  return p;
}

inline long& Log::rotateBytes() {
  static long n = 50L * 1024 * 1024; // 缺省 50MB
  return n;
}

inline int& Log::keepCount() {
  static int k = 5;
  return k;
}

inline void Log::rotateLocked() {
  // 假定已持锁。轮转链：删除最老 .keep，.i -> .i+1 倒序挪动，主文件 -> .1
  if (filePath().empty() || keepCount() <= 0) return;
  FILE* f = file();
  if (f != nullptr && f != stderr) std::fclose(f);
  const std::string base = filePath();
  std::remove((base + "." + std::to_string(keepCount())).c_str());
  for (int i = keepCount() - 1; i >= 1; --i) {
    std::rename((base + "." + std::to_string(i)).c_str(),
                (base + "." + std::to_string(i + 1)).c_str());
  }
  std::rename(base.c_str(), (base + ".1").c_str());
  FILE* nf = std::fopen(base.c_str(), "a");
  file() = (nf != nullptr) ? nf : stderr; // 重开失败退回 stderr（不丢日志）
  if (nf == nullptr) {
    // 注意：此处 LOG_E 宏尚未定义（位于文件末尾），直接 fprintf
    std::fprintf(stderr, "[ERROR] [log] 轮转后重开日志文件失败: %s\n", base.c_str());
  }
}

inline void Log::setLevel(Level lv) { threshold() = lv; }

inline void Log::setFile(FILE* f) {
  std::lock_guard<std::mutex> lock(mutex());
  if (file() != nullptr && file() != stderr) std::fclose(file());
  file() = (f != nullptr) ? f : stderr;
  filePath().clear(); // 外部 setFile（单测）不参与轮转
}

inline void Log::shutdown() { setFile(stderr); }

inline void Log::init() {
  if (const char* v = std::getenv("IM_LOG_LEVEL")) {
    if (std::strcmp(v, "DEBUG") == 0) setLevel(Level::DEBUG);
    else if (std::strcmp(v, "INFO") == 0) setLevel(Level::INFO);
    else if (std::strcmp(v, "WARN") == 0) setLevel(Level::WARN);
    else if (std::strcmp(v, "ERROR") == 0) setLevel(Level::ERROR);
  }
  if (const char* v = std::getenv("IM_LOG_ROTATE_BYTES")) {
    rotateBytes() = std::atol(v); // 0 = 不轮转
  }
  if (const char* v = std::getenv("IM_LOG_KEEP")) {
    const int k = std::atoi(v);
    if (k > 0) keepCount() = k;
  }
  if (const char* v = std::getenv("IM_LOG_FILE")) {
    if (v[0] != '\0') {
      FILE* f = std::fopen(v, "a");
      if (f != nullptr) {
        {
          // 路径记录与文件设置需在同一临界区（write 侧锁内读路径）
          std::lock_guard<std::mutex> lock(mutex());
          filePath() = v;
          file() = f;
        }
      }
    }
  }
}

inline void Log::write(Level lv, const char* path, int line, const std::string& msg) {
  if (lv < threshold()) return;
  std::time_t t = std::time(nullptr);
  char ts[32] = {0};
  std::tm tmv{};
  // 日志时间固定 UTC+8（北京时间），不依赖系统/容器时区：
  // 部署容器默认 UTC，localtime_r 会让日志时间慢 8 小时。
  // 显式加 8 小时偏移后用 gmtime_r，任何环境（本地 WSL / Docker /
  // 云主机任意时区）日志时间恒为北京时间
  const std::time_t tBeijing = t + 8 * 3600;
  gmtime_r(&tBeijing, &tmv);
  std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
  const char* base = std::strrchr(path, '/');
  base = (base != nullptr) ? base + 1 : path;
  const size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
  std::lock_guard<std::mutex> lock(mutex());
  std::fprintf(file(), "%s [%s] [t%zx] %s:%d %s\n", ts, levelName(lv), tid, base, line,
               msg.c_str());
  std::fflush(file());
  // 按大小轮转：仅在 init 配置了文件路径且超阈值时触发（锁内执行）
  if (rotateBytes() > 0 && !filePath().empty() && file() != stderr) {
    if (std::ftell(file()) > rotateBytes()) rotateLocked();
  }
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
