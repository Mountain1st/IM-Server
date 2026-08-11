#pragma once

#include "channel.hpp"
#include "eventloop.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace im {

class EventLoopThreadPool;
class ThreadPool;
class Connection;

// 网络层：拥有一个主 eventloop（绑定监听 fd，负责 accept）和
// 一组从 eventloop（线程池，负责与各个客户端实际通信）。
// 新连接按 fd % 从线程数 分配到对应从 loop。
// 不含业务逻辑与工作线程；业务由上层（AppServer）通过回调接入。
class TcpServer : public std::enable_shared_from_this<TcpServer> {
public:
  TcpServer(uint16_t port, size_t ioThreads);
  TcpServer(uint16_t port, size_t ioThreads, size_t maxConnections);
  ~TcpServer();

  using ConnectionPtr = std::shared_ptr<Connection>;
  // 回调在连接所属的从 loop 线程执行；参数携带该 loop
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

  // 启动从 eventloop 线程池并开始监听（主 loop 由 run() 运行）
  void start();
  // 运行主 eventloop（阻塞，应在主线程调用）
  void run();
  // 最小停止请求：仅唤醒并退出主 eventloop（async-signal-safe，
  // 供信号处理器调用；完整优雅关闭 stop() 在 run() 返回后由主线程执行）
  void requestStop();
  // 通知主从 eventloop 停止，并关闭全部现存连接
  void stop();
  void removeConnection(const ConnectionPtr& conn);

  EventLoop* mainLoop() { return &acceptLoop_; }

private:
  void handleAccept();
  void deviceNewConnection(int cfd);

  EventLoop acceptLoop_;  // 主 eventloop：绑定监听 fd，负责 accept
  std::unique_ptr<EventLoopThreadPool> ioPool_; // 从 eventloop 线程池：与客户端通信
  int listenFd_;
  Channel acceptChannel_;
  std::mutex mutex_;
  std::unordered_set<ConnectionPtr> connections_;
  size_t maxConnections_ = 0; // 0 表示不限

  NewConnectionCallback newConnCb_;
  ConnectionClosedCallback connClosedCb_;
};

} // namespace im