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
  // 复用带超时的优雅停止（drain 语义；超时放弃剩余任务防析构挂死）
  stop();
}

void ThreadPool::start() {
  for (size_t i = 0; i < threads_; ++i) {
    workers_.emplace_back([this] { workerLoop(); });
    LOG_I("[pool] 工作线程-" << (i + 1) << " 创建完成 (共 " << threads_ << ")");
  }
}

void ThreadPool::stop(uint32_t drainTimeoutMs) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) return; // 幂等：重复停止无害
    stop_ = true;
  }
  cv_.notify_all();
  {
    // 等全部 worker 退出：正常路径=队列清空（drain 完成）；
    // 超时路径=置 abandon，worker 做完手头任务即退（当前任务不可中断，
    // 但任务内部均有 IO 超时，等待有界）
    std::unique_lock<std::mutex> lock(mutex_);
    if (!doneCv_.wait_for(lock, std::chrono::milliseconds(drainTimeoutMs),
                          [this] { return finished_ == workers_.size(); })) {
      LOG_W("[pool] 排水超时（" << drainTimeoutMs << "ms），放弃剩余 "
            << tasks_.size() << " 个任务");
      abandon_ = true;
      cv_.notify_all();
    }
  }
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
      cv_.wait(lock, [this] { return stop_ || abandon_ || !tasks_.empty(); });
      // 超时强退：丢弃剩余队列（当前任务除外）
      if (abandon_) break;
      // 正常停止：drain 语义——队列清空才退出
      if (stop_ && tasks_.empty()) break;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
  // 退出计数（锁内）并唤醒等待方（stop 的 drain 完成判定）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++finished_;
  }
  doneCv_.notify_all();
}

} // namespace im