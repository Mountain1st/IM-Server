#pragma once

#include "eventloop.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace im {

// 一个运行独立 EventLoop 的线程（IO 线程）。
class EventLoopThread {
public:
    EventLoopThread();
    ~EventLoopThread();

    // 启动线程并在其中运行 EventLoop；返回该 loop 指针
    EventLoop* startLoop();

    // 当前线程是否是本 IO 线程
    bool isSelf() const;
    EventLoop* loop() const { return loop_; }

private:
    void threadFunc();

    EventLoop* loop_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool started_ = false;
};

} // namespace im