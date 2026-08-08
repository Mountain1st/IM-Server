#pragma once

#include "channel.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace im {

class EventLoop;
class EventLoopThreadPool;
class Connection;

// 监听、接受连接，并把每个连接交付给 IO 线程池中的一个 EventLoop 处理。
class TcpServer : public std::enable_shared_from_this<TcpServer> {
public:
    TcpServer(EventLoop* loop, uint16_t port, EventLoopThreadPool* ioPool);
    ~TcpServer();

    using ConnectionPtr = std::shared_ptr<Connection>;
    // 回调在连接所属的 IO loop 线程执行；参数携带该 loop
    using NewConnectionCallback =
        std::function<void(EventLoop*, const ConnectionPtr&)>;
    using ConnectionClosedCallback =
        std::function<void(const ConnectionPtr&)>;

    void setNewConnectionCallback(NewConnectionCallback cb) {
        newConnCb_ = std::move(cb);
    }
    void setConnectionClosedCallback(ConnectionClosedCallback cb) {
        connClosedCb_ = std::move(cb);
    }

    void start();
    void removeConnection(const ConnectionPtr& conn);

private:
    void handleAccept();
    void deviceNewConnection(int cfd);

    EventLoop* loop_;
    EventLoopThreadPool* ioPool_;
    int listenFd_;
    Channel acceptChannel_;
    std::mutex mutex_;
    std::unordered_set<ConnectionPtr> connections_;

    NewConnectionCallback newConnCb_;
    ConnectionClosedCallback connClosedCb_;
};

} // namespace im