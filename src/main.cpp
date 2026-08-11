#include "appserver.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

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

// 用法: imserver [端口] [IO线程数] [工作线程数] [最大连接数]
// 缺省: 端口 9191，IO 线程 3，工作线程 5，连接数 0(不限)
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

  // 业务层：内部启动工作线程并通知 TcpServer（其负责主从 eventloop）
  auto server = std::make_shared<im::AppServer>(port, ioThreads, workThreads, maxConnections);
  g_server = server.get();
  server->start();

  struct sigaction sa{};
  sa.sa_handler = &stop;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::signal(SIGPIPE, SIG_IGN);

  std::cout << "[server] 启动完成，按 Ctrl+C 退出" << std::endl;
  server->run();
  // run() 返回即表示收到停止请求，在主线程执行完整优雅关闭：
  // 停止工作线程 → 通知 TcpServer 停止（主从 eventloop → 关闭连接）
  server->stop();
  std::cout << "[server] 已退出" << std::endl;
  return 0;
}