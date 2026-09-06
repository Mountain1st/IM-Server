#include "threadpool.hpp"

#include "log.hpp"
#include "util.hpp"

#include <iostream>
#include <utility>

namespace im {

ThreadPool::ThreadPool(size_t threads) : threads_(threads) {
  workers_.reserve(threads);
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& w : workers_) {
    if (w.joinable()) w.join();
  }
}

void ThreadPool::start() {
  for (size_t i = 0; i < threads_; ++i) {
    workers_.emplace_back([this] { workerLoop(); });
    LOG_I("[pool] 工作线程-" << (i + 1) << " 创建完成 (共 " << threads_ << ")");
  }
}

void ThreadPool::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& w : workers_) {
    if (w.joinable()) w.join();
  }
}

void ThreadPool::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  cv_.notify_one();
}

void ThreadPool::workerLoop() {
  // 阻塞停止信号：Ctrl+C 只能由主线程处理，避免信号处理器
  // 在工作线程内触发 self-join 死锁等未定义行为
  blockStopSignals();
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
      if (stop_ && tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

} // namespace im