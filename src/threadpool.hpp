#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace im {

// 固定线程数的工作线程池。提交的任务在当前线程池中串行或并行执行。
class ThreadPool {
public:
  explicit ThreadPool(size_t threads);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // 创建线程并开始消费任务（构造时不创建，由拥有者显式启动）
  void start();
  void submit(std::function<void()> task);
  void stop();
  size_t size() const { return workers_.size(); }

private:
  void workerLoop();

  size_t threads_;
  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
};

} // namespace im