// ============================================================
// main：进程入口（启动参数/环境变量解析 → 组装 AppServer → 信号挂接）
// 启动参数：端口 9191 / IO 线程 3 / 工作线程 5 / 最大连接 0(不限) /
//           Origin 白名单（逗号分隔，缺省不校验）
// 环境变量：IM_MYSQL_*（必需，密码无缺省——脱敏改造）/ IM_REDIS_*
//           （IM_REDIS_ENABLED=1 启用）/ IM_MAX_PER_IP_CONN /
//           IM_TRUSTED_PROXIES / IM_LOG_*
// 信号：SIGINT/SIGTERM 处理器只做 async-signal-safe 的两件事
// （置标志 + requestStop 唤醒主 loop）；完整优雅停机在 run() 返回后
// 的主线程执行（四步：停接入 → 1001 → 排水 → 兜底）
// ============================================================
#include "appserver.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "log.hpp"

namespace {
im::AppServer* g_server = nullptr;
volatile std::sig_atomic_t g_stop_requested = 0;

// 信号处理器：仅做两件 async-signal-safe 的事——
// 置标志 + 唤醒主事件循环。完整关闭放在 run() 返回后的主线程中执行。
void stop(int) {
  g_stop_requested = 1;
  if (g_server) g_server->requestStop();
}
} // namespace

// 用法: imserver [端口] [IO线程数] [工作线程数] [最大连接数] [允许Origin列表]
// 缺省: 端口 9191，IO 线程 3，工作线程 5，连接数 0(不限)
// 允许Origin列表: 逗号分隔，条目可为 "scheme://host[:port]" 或裸 "host[:port]"
// （如 "http://localhost:8080,localhost:9191"）；缺省不校验 Origin/Host。
int main(int argc, char* argv[]) {
  unsigned short port = 9191;
  unsigned ioThreads = 3;
  unsigned workThreads = 5;
  size_t maxConnections = 0;

  auto parseUint = [](const char* s) -> unsigned {
    try {
      return static_cast<unsigned>(std::stoul(s));
    } catch (...) {
      return 0;
    }
  };

  if (argc > 1) {
    unsigned p = parseUint(argv[1]);
    port = (p > 0 && p < 65536) ? static_cast<unsigned short>(p) : 9191;
  }
  if (argc > 2) {
    unsigned v = parseUint(argv[2]);
    if (v > 0 && v <= 64) ioThreads = v;
  }
  if (argc > 3) {
    unsigned v = parseUint(argv[3]);
    if (v > 0 && v <= 256) workThreads = v;
  }
  if (argc > 4) {
    maxConnections = parseUint(argv[4]);
  }

  // 允许的 Origin/Host 列表（逗号分隔，可省略 scheme）；缺省为空 → 不校验
  std::vector<std::string> allowedOrigins;
  if (argc > 5) {
    std::string list = argv[5];
    size_t pos = 0;
    for (;;) {
      size_t comma = list.find(',', pos);
      std::string item = list.substr(
          pos, comma == std::string::npos ? std::string::npos : comma - pos);
      size_t start = item.find_first_not_of(" \t");
      size_t end = item.find_last_not_of(" \t");
      if (start != std::string::npos) {
        allowedOrigins.push_back(item.substr(start, end - start + 1));
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    if (!allowedOrigins.empty()) {
      std::string origins;
      for (const auto& o : allowedOrigins) origins += o + " ";
      LOG_I("[server] 握手允许 Origin/Host: " << origins);
    }
  }

  // 用户存储后端：缺省连接开发库 IMS_reactor_db（用户 IMS_jack）。
  // 可用 IM_MYSQL_HOST/PORT/USER/PASSWORD/DB 环境变量覆盖；连接失败将启动报错。
  im::UserStoreConfig userConfig;
  if (const char* v = std::getenv("IM_MYSQL_DB")) userConfig.mysqlDb = v;
  if (const char* v = std::getenv("IM_MYSQL_HOST")) userConfig.mysqlHost = v;
  if (const char* v = std::getenv("IM_MYSQL_PORT")) userConfig.mysqlPort = static_cast<unsigned>(std::atoi(v));
  if (const char* v = std::getenv("IM_MYSQL_USER")) userConfig.mysqlUser = v;
  if (const char* v = std::getenv("IM_MYSQL_PASSWORD")) userConfig.mysqlPassword = v;

  // Redis 存储（token 等）：IM_REDIS_ENABLED=1 时启用；
  // 其余由 IM_REDIS_HOST/PORT/PASSWORD/DB 覆盖（本环境 Redis 容器端口 6380）。
  im::RedisStoreConfig redisConfig;
  if (const char* v = std::getenv("IM_REDIS_ENABLED")) redisConfig.enabled = (std::string(v) == "1" || std::string(v) == "true");
  if (const char* v = std::getenv("IM_REDIS_HOST")) redisConfig.redisHost = v;
  if (const char* v = std::getenv("IM_REDIS_PORT")) redisConfig.redisPort = static_cast<unsigned>(std::atoi(v));
  if (const char* v = std::getenv("IM_REDIS_PASSWORD")) redisConfig.redisPassword = v;
  if (const char* v = std::getenv("IM_REDIS_DB")) redisConfig.redisDb = std::atoi(v);

  // 日志初始化（环境变量 IM_LOG_FILE / IM_LOG_LEVEL，见 src/log.hpp）
  im::log::Log::init();

  // 单 IP 并发连接上限：IM_MAX_PER_IP_CONN 覆盖（缺省 100；0 表示不限）
  size_t maxPerIpConn = im::config::kMaxPerIpConnections;
  if (const char* v = std::getenv("IM_MAX_PER_IP_CONN")) {
    maxPerIpConn = static_cast<size_t>(std::atoi(v));
  }

  // 可信反向代理列表：IM_TRUSTED_PROXIES（逗号分隔 IP）。对端在列表内时，
  // 握手日志显示 X-Forwarded-For 的真实客户端 IP；缺省为空 = 忽略该头（防伪造）。
  std::vector<std::string> trustedProxies;
  if (const char* v = std::getenv("IM_TRUSTED_PROXIES")) {
    std::string list = v;
    size_t pos = 0;
    for (;;) {
      size_t comma = list.find(',', pos);
      std::string item = list.substr(
          pos, comma == std::string::npos ? std::string::npos : comma - pos);
      size_t start = item.find_first_not_of(" \t");
      size_t end = item.find_last_not_of(" \t");
      if (start != std::string::npos) {
        trustedProxies.push_back(item.substr(start, end - start + 1));
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    if (!trustedProxies.empty()) {
      std::string joined;
      for (const auto& p : trustedProxies) joined += p + " ";
      LOG_I("[server] 可信代理（信任 X-Forwarded-For）: " << joined);
    }
  }

  // 业务层：内部启动工作线程并通知 TcpServer（其负责主从 eventloop）。
  // 用户存储为 MySQL：配置缺失或连接失败时抛异常，这里捕获并明确报错退出。
  std::shared_ptr<im::AppServer> server;
  try {
    server = std::make_shared<im::AppServer>(
        port, ioThreads, workThreads, maxConnections, allowedOrigins, userConfig, redisConfig,
        maxPerIpConn, trustedProxies);
  } catch (const std::exception& e) {
    LOG_E("[server] 启动失败: " << e.what());
    return 1;
  }
  g_server = server.get();
  server->start();

  struct sigaction sa{};
  sa.sa_handler = &stop;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::signal(SIGPIPE, SIG_IGN);

  LOG_I("[server] 启动完成，按 Ctrl+C 退出");
  server->run();
  // run() 返回即表示收到停止请求，在主线程执行完整优雅关闭：
  // 停止接入 → 向在线会话发 Close(1001) → 工作队列排水（带超时）→ 兜底关闭
  // 看门狗：优雅停机各环节均有超时，理论上 15 秒内必然完成；若出现
  // 未预见的挂点（历史事件：ASan 轮疑似挂 31 分钟），15 秒后硬退
  // 保证进程可退出（compose restart 依赖进程退出才能拉起）
  {
    std::thread watchdog([] {
      std::this_thread::sleep_for(std::chrono::seconds(15));
      LOG_E("[server] 停机看门狗超时，硬退出（优雅关闭未在 15 秒内完成）");
      ::_exit(0);
    });
    watchdog.detach();
  }
  server->stop();
  LOG_I("[server] 已退出");
  return 0;
}