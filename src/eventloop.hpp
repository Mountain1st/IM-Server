#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace im {

class Channel;
class Connection;

// epoll ET 事件循环。所有注册在其中的 Channel 的回调只在 run() 所在线程触发。
// 其它线程通过 wakeup() / postToLoop() 通知 loop 线程处理任务。
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void run();
    void stop();

    // 增/改某个 fd 的 ET 事件关注（应由本 loop 线程调用）
    void update(Channel* ch);
    // 移除某个 fd 的事件关注（应由本 loop 线程调用）
    void remove(Channel* ch);

    // 若当前线程就是本 loop 线程则直接执行，否则投递并唤醒 loop 线程执行
    void runInLoop(std::function<void()> task);
    // 唤醒 loop 线程（线程安全，可被任意线程调用）
    void wakeup();
    // 把一个要在 loop 线程执行的函数投递过去（线程安全）
    void postToLoop(std::function<void()> task);
    // 通知 loop 线程稍后 flush 一个连接的待写数据（线程安全）
    void addPendingFlush(Connection* conn);

    bool isInLoopThread() const;
    std::thread::id threadId() const { return threadId_; }

private:
    void handlePending();

    int epfd_;
    int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;
    std::map<int, Channel*> channels_;
    bool running_ = false;
    std::thread::id threadId_{};

    std::mutex taskMutex_;
    std::vector<std::function<void()>> tasks_;
    std::mutex flushMutex_;
    std::vector<Connection*> pendingFlush_;
    std::mutex updateMutex_;
};

} // namespace im