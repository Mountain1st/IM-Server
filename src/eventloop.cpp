#include "eventloop.hpp"

#include "channel.hpp"
#include "connection.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <thread>

namespace im {

EventLoop::EventLoop() {
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ == -1) {
        std::cerr << "[eventloop] epoll_create1 失败: " << std::strerror(errno) << std::endl;
        ::exit(1);
    }
    wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd_ == -1) {
        std::cerr << "[eventloop] eventfd 失败: " << std::strerror(errno) << std::endl;
        ::exit(1);
    }
    wakeupChannel_ = std::make_unique<Channel>(wakeupFd_);
    wakeupChannel_->setReadCallback([this] {
        uint64_t one;
        // 清空 eventfd 计数，避免无限唤醒
        while (::read(wakeupFd_, &one, sizeof one) > 0) {}
    });
    wakeupChannel_->enableReading(this);
}

EventLoop::~EventLoop() {
    ::close(epfd_);
    ::close(wakeupFd_);
}

void EventLoop::run() {
    threadId_ = std::this_thread::get_id();
    running_ = true;
    std::vector<struct epoll_event> events(256);
    while (running_) {
        int n = ::epoll_wait(epfd_, events.data(),
                             static_cast<int>(events.size()), -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "[eventloop] epoll_wait 失败: " << std::strerror(errno) << std::endl;
            break;
        }
        for (int i = 0; i < n; ++i) {
            auto* ch = static_cast<Channel*>(events[i].data.ptr);
            ch->handleEvent(events[i].events);
        }
        handlePending();
    }
}

void EventLoop::stop() {
    running_ = false;
    wakeup();
}

bool EventLoop::isInLoopThread() const {
    return threadId_ == std::this_thread::get_id();
}

void EventLoop::runInLoop(std::function<void()> task) {
    if (isInLoopThread()) {
        task();
    } else {
        postToLoop(std::move(task));
    }
}

void EventLoop::update(Channel* ch) {
    struct epoll_event ev{};
    ev.data.ptr = ch;
    ev.events = EPOLLET; // 边沿触发
    if (ch->isReading()) ev.events |= EPOLLIN | EPOLLRDHUP;
    if (ch->isWriting()) ev.events |= EPOLLOUT;
    if (channels_.count(ch->fd())) {
        ::epoll_ctl(epfd_, EPOLL_CTL_MOD, ch->fd(), &ev);
    } else {
        channels_[ch->fd()] = ch;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, ch->fd(), &ev);
    }
}

void EventLoop::remove(Channel* ch) {
    channels_.erase(ch->fd());
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, ch->fd(), nullptr);
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    // ET 下写入 eventfd 唤醒 loop；忽略 EAGAIN（已挂起一次唤醒）
    ssize_t r = ::write(wakeupFd_, &one, sizeof one);
    (void)r;
}

void EventLoop::postToLoop(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        tasks_.push_back(std::move(task));
    }
    wakeup();
}

void EventLoop::addPendingFlush(Connection* conn) {
    {
        std::lock_guard<std::mutex> lock(flushMutex_);
        pendingFlush_.push_back(conn);
    }
    wakeup();
}

void EventLoop::handlePending() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        tasks.swap(tasks_);
    }
    for (auto& t : tasks) t();

    std::vector<Connection*> flush;
    {
        std::lock_guard<std::mutex> lock(flushMutex_);
        flush.swap(pendingFlush_);
    }
    for (auto* conn : flush) conn->flushWrite();
}

} // namespace im