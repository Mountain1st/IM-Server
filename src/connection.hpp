#pragma once

#include "channel.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace im {

class EventLoop;

// 一个已建立的 TCP 连接。
// 读/写 I/O 均在 EventLoop 线程执行（ET 语义）；
// send() 可被任意线程调用（跨线程安全），实际发送由 loop 线程 flush。
class Connection : public std::enable_shared_from_this<Connection> {
public:
  Connection(EventLoop* loop, int fd);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  int fd() const { return fd_; }
  EventLoop* loop() { return loop_; }

  void setMessageCallback(std::function<void(const std::shared_ptr<Connection>&, std::string)> cb) {
    messageCb_ = std::move(cb);
  }
  void setCloseCallback(std::function<void(const std::shared_ptr<Connection>&)> cb) {
    closeCb_ = std::move(cb);
  }

  // loop 线程：登记到 epoll，开始读
  void start();
  // 任意线程：追加发送数据，由 loop 线程异步 flush（线程安全）
  void send(const char* data, size_t len);
  void send(const std::string& data) { send(data.data(), data.size()); }
  // 任意线程：安排关闭（线程安全，实际 close 在 loop 线程执行）
  void shutdown();
  // 必须在本连接所属的 loop 线程调用：同步关闭（服务端停止时用于已停 loop 的连接）
  void forceClose();
  // loop 线程：发送缓冲全部落盘（由 pendingFlush 驱动）
  void flushWrite();

  bool isClosed() const { return closed_.load(); }

private:
  void handleRead();
  void handleError();
  void closeInLoop();

  EventLoop* loop_;
  int fd_;
  Channel channel_;
  std::atomic<bool> closed_{false};

  // 读缓冲：仅 loop 线程访问
  std::string inBuffer_;
  std::function<void(const std::shared_ptr<Connection>&, std::string)> messageCb_;
  std::function<void(const std::shared_ptr<Connection>&)> closeCb_;

  // 写缓冲：可被任意线程追加，loop 线程消费
  std::mutex writeMutex_; // 保护 writeQueue_
  std::deque<std::string> writeQueue_;
  std::string writeChunk_; // 当前正在写的数据（仅 loop 线程访问）
  size_t writeChunkOffset_ = 0; // writeChunk_ 已发送的偏移（仅 loop 线程访问）
  // 写缓冲总字节数（writeQueue_ + writeChunk_）：原子计数。
  // 任意线程的 send() 用它判断上限（不再读 writeChunk_，消除跨线程竞争），
  // loop 线程 flushWrite 发送成功后递减。
  std::atomic<size_t> writeBufferedBytes_{0};
  // 写缓冲曾超限，置位后 send() 直接返回（触发断开的线程负责调用 shutdown）
  std::atomic<bool> queuedTooMuch_{false};
};

} // namespace im