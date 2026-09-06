#include "eventloop_thread.hpp"

#include "log.hpp"
#include "util.hpp"

#include <iostream>
#include <memory>
#include <thread>

namespace im {

EventLoopThread::EventLoopThread() = default;

EventLoopThread::~EventLoopThread() {
  if (loop_) loop_->stop();
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
    started_ = true;
  }
  cv_.notify_one();
  loop.run();
  loop_ = nullptr;
}

bool EventLoopThread::isSelf() const {
  return thread_.get_id() == std::this_thread::get_id();
}

} // namespace im