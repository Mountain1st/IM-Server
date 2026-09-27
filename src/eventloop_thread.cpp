#include "eventloop_thread.hpp"

#include "log.hpp"
#include "util.hpp"

#include <iostream>
#include <memory>
#include <thread>

namespace im {

EventLoopThread::EventLoopThread() = default;

EventLoopThread::~EventLoopThread() {
  {
    // 锁内读 loop_：与 threadFunc 退出时的锁内清空互斥（TSan 竞争修复，
    // 此前为无锁读 vs 锁外写）
    std::lock_guard<std::mutex> lock(mutex_);
    if (loop_) loop_->stop();
  }
  if (thread_.joinable()) thread_.join();
}

EventLoop* EventLoopThread::startLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  thread_ = std::thread([this] { threadFunc(); });
  cv_.wait(lock, [this] { return started_; });
  return loop_;
}

void EventLoopThread::threadFunc() {
  // 阻塞停止信号：保证 SIGINT/SIGTERM 只会投递到主线程
  blockStopSignals();
  LOG_I("[io] IO 线程创建, id=" << std::this_thread::get_id());
  EventLoop loop;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = &loop;
    // 锁内发布线程身份：started_ 的置位与主线程 cv 等待构成同步边，
    // 保证主线程此后调用 isInLoopThread/runInLoop 读到的身份是已发布的
    loop.publishThisThread();
    started_ = true;
  }
  cv_.notify_one();
  loop.run();
  {
    // 锁内清空：与析函数的锁内判空互斥
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
  }
}

bool EventLoopThread::isSelf() const {
  return thread_.get_id() == std::this_thread::get_id();
}

} // namespace im