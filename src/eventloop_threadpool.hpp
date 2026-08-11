#pragma once

#include <atomic>
#include <memory>
#include <vector>

namespace im {

class EventLoop;
class EventLoopThread;

// IO 线程池：管理多个 EventLoopThread，用于让多个连接分属不同 reactor。
class EventLoopThreadPool {
public:
  explicit EventLoopThreadPool(size_t numThreads);
  ~EventLoopThreadPool();

  // 启动所有 IO 线程（返回前所有 loop 已就绪）
  void start();
  // 停止所有从 loop（线程安全，可被任意线程调用）
  void stop();

  // 按序号取从 loop；未启动时返回 nullptr
  EventLoop* getLoop(size_t index);
  // 轮询（round-robin）获取下一个 IO loop
  EventLoop* getNextLoop();
  size_t size() const { return threads_.size(); }

private:
  std::vector<std::unique_ptr<EventLoopThread>> threads_;
  std::vector<EventLoop*> loops_;
  std::atomic<size_t> next_{0};
};

} // namespace im