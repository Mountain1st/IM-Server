#pragma once

#include "server_config.hpp"

#include <condition_variable>
#include <cstdint>
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
  // 优雅停止：置停止位后唤醒全部 worker——worker 把队列中已入队任务
  // 全部执行完才退出（drain 语义）。drainTimeoutMs 内未完成则放弃剩余
  // 任务（abandon），worker 做完手头任务即退出（当前任务不可中断，
  // 最坏再等一个任务时长——任务内部均有 IO 超时，有界）。
  void stop(uint32_t drainTimeoutMs = config::kPoolDrainTimeoutMs);
  size_t size() const { return workers_.size(); }

private:
  void workerLoop();

  size_t threads_;
  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;    // 唤醒 worker 取任务/退出
  std::condition_variable doneCv_; // 通知 stop 等待方：又有 worker 退出
  size_t finished_ = 0;   // 已退出的 worker 数（锁内维护）
  bool stop_ = false;     // 停止位：不再接受"必须清空队列"的退出条件
  bool abandon_ = false;  // 超时强退位：丢弃剩余队列直接退出
};

} // namespace im