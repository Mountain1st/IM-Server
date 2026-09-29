// ============================================================
// EventLoopThreadPool：IO 线程池（从 eventloop 集合的容器）
// - start：逐个启动 EventLoopThread 并收集 loop 指针（此后只读）
// - getLoop(i)：按下标取（越界取模）
// - getNextLoop：轮询分配（当前未被使用——TcpServer 用最少连接优先）
// - stop：逐个 stopLoop（各自锁内置空 + join）
// ============================================================
#include "eventloop_threadpool.hpp"

#include "eventloop.hpp"
#include "eventloop_thread.hpp"

namespace im {

EventLoopThreadPool::EventLoopThreadPool(size_t numThreads) {
  threads_.reserve(numThreads);
  for (size_t i = 0; i < numThreads; ++i) {
    threads_.emplace_back(std::make_unique<EventLoopThread>());
  }
}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start() {
  loops_.clear();
  loops_.reserve(threads_.size());
  for (auto& t : threads_) {
    loops_.push_back(t->startLoop());
  }
}

void EventLoopThreadPool::stop() {
  for (auto* l : loops_) {
    if (l) l->stop();
  }
}

EventLoop* EventLoopThreadPool::getLoop(size_t index) {
  if (loops_.empty()) return nullptr;
  return loops_[index % loops_.size()];
}

EventLoop* EventLoopThreadPool::getNextLoop() {
  if (loops_.empty()) return nullptr;
  size_t idx = next_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
  return loops_[idx];
}

} // namespace im