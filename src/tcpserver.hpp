#pragma once

#include "channel.hpp"
#include "eventloop.hpp"
#include "server_config.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace im {

class EventLoopThreadPool;
class ThreadPool;
class Connection;

// 网络层：拥有一个主 eventloop（绑定监听 fd，负责 accept）和
// 一组从 eventloop（线程池，负责与各个客户端实际通信）。
// 新连接按最少连接优先（least-connections）分配到负载最小的从 loop。
// 不含业务逻辑与工作线程；业务由上层（AppServer）通过回调接入。
class TcpServer : public std::enable_shared_from_this<TcpServer> {
public:
  TcpServer(uint16_t port, size_t ioThreads);
  TcpServer(uint16_t port, size_t ioThreads, size_t maxConnections,
            size_t maxPerIpConnections = config::kMaxPerIpConnections);
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
  // 优雅停机第一步（调用前提：主 loop 已停，从 loop 仍运行）：
  // 向所有在线连接发送 WS Close(1001 Going Away) 并按"写完即关"关闭。
  // 从 loop 归属的连接经各自 loop 线程处理（barrier 等下发完成）；
  // 主 loop 归属的连接直接关闭（主 loop 已停无法走帧，仅无 IO 线程的
  // 退化配置会出现）。完成后由上层继续排水工作队列，最后调 stop() 兜底。
  void gracefulShutdown();
  void removeConnection(const ConnectionPtr& conn);

  EventLoop* mainLoop() { return &acceptLoop_; }

  // 从各 IO loop 的连接计数中选负载最小者的下标（平局取最靠前）。
  // 纯函数、static：供单测直接验证分配策略
  static size_t pickLeastLoadedIndex(const std::vector<std::atomic<size_t>>& counts) {
    size_t best = 0;
    for (size_t i = 1; i < counts.size(); ++i) {
      if (counts[i].load(std::memory_order_relaxed) <
          counts[best].load(std::memory_order_relaxed)) {
        best = i;
      }
    }
    return best;
  }

private:
  struct Listener {
    int fd = -1;
    Channel channel{-1};
  };
  // 追加一个监听器并绑定/监听；成功返回 true
  bool addListener(int family, bool ipv6Only, uint16_t port,
                   const std::string& label);
  void handleAccept();
  void deviceNewConnection(int cfd, const std::string& peerIp);

  EventLoop acceptLoop_;  // 主 eventloop：绑定监听 fd，负责 accept
  std::unique_ptr<EventLoopThreadPool> ioPool_; // 从 eventloop 线程池：与客户端通信
  std::vector<Listener> listeners_; // IPv4 与 IPv6 各一个监听器
  std::mutex mutex_;
  std::unordered_set<ConnectionPtr> connections_;
  size_t maxConnections_ = 0; // 0 表示不限
  size_t maxPerIpConnections_ = config::kMaxPerIpConnections; // 0 表示不限
  std::unordered_map<std::string, size_t> perIpCounts_; // IP -> 并发连接数
  std::unordered_map<Connection*, std::string> connIp_; // 连接 -> IP（释放时扣减）
  // IO loop -> 下标（start 后建立，只读）；连接归属其扣减 per-loop 计数用
  std::unordered_map<EventLoop*, size_t> loopIndex_;
  // 各 IO loop 当前连接数（accept 主线程增、各 IO 线程减，原子操作）
  std::vector<std::atomic<size_t>> loopCounts_;

  NewConnectionCallback newConnCb_;
  ConnectionClosedCallback connClosedCb_;
};

} // namespace im