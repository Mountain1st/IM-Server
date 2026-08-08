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

    void submit(std::function<void()> task);
    size_t size() const { return workers_.size(); }

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace im