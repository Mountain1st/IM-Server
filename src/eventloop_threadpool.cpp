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

EventLoop* EventLoopThreadPool::getNextLoop() {
    if (loops_.empty()) return nullptr;
    size_t idx = next_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    return loops_[idx];
}

} // namespace im