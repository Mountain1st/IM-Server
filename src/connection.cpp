#include "connection.hpp"

#include "eventloop.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "server_config.hpp"
#include "util.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace im {

namespace {
constexpr size_t kReadBufferSize = config::kReadBufferSize;
constexpr size_t kMaxReadPerEvent = config::kMaxReadPerEvent;
constexpr size_t kMaxWriteBufferBytes = config::kMaxWriteBufferBytes;
}

Connection::Connection(EventLoop* loop, int fd)
  : loop_(loop), fd_(fd), channel_(fd) {
  channel_.setReadCallback([this] { handleRead(); });
  channel_.setCloseCallback([this] { handleError(); });
}

Connection::~Connection() {
  if (fd_ >= 0) ::close(fd_);
}

void Connection::start() {
  // write 回调用弱引用捕获：同一批 epoll 事件中 readCb 可能已销毁本连接
  // （对端 FIN/RST 触发 handleRead 关闭），此时 EPOLLOUT 分支仍会调用 writeCb。
  // 若直接捕获裸 this 将导致对已释放对象的 UB；弱引用 + lock 则安全跳过。
  std::weak_ptr<Connection> weak = shared_from_this();
  channel_.setWriteCallback([weak] {
    if (auto self = weak.lock()) self->flushWrite();
  });
  channel_.enableReading(loop_);
}

Connection::SendResult Connection::send(const char* data, size_t len) {
  if (closed_.load() || queuedTooMuch_.load()) {
    return closed_.load() ? SendResult::Closed : SendResult::Overflow;
  }
  // 先原子判断是否超限。writeBufferedBytes_ 是唯一权威的字节计数，
  // 不再读取 writeChunk_（其仅由 loop 线程修改），消除跨线程数据竞争。
  size_t prev = writeBufferedBytes_.fetch_add(len, std::memory_order_relaxed);
  if (prev + len > kMaxWriteBufferBytes) {
    // 超限：回滚计数并标记，避免重复触发断开
    writeBufferedBytes_.fetch_sub(len, std::memory_order_relaxed);
    queuedTooMuch_.store(true, std::memory_order_relaxed);
    g_metrics.writeOverflows.fetch_add(1, std::memory_order_relaxed);
    LOG_E("[conn] fd=" << fd_ << " 写缓冲超限，断开慢消费/不可达连接");
    shutdown();
    return SendResult::Overflow;
  }
  {
    std::lock_guard<std::mutex> lock(writeMutex_);
    writeQueue_.emplace_back(data, len);
  }
  auto self = shared_from_this();
  loop_->addPendingFlush(std::move(self));
  return SendResult::Ok;
}

void Connection::flushWrite() {
  if (closed_.load() || queuedTooMuch_.load()) return;

  for (;;) {
    if (writeChunk_.empty()) {
      {
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (writeQueue_.empty()) break;
        writeChunk_ = std::move(writeQueue_.front());
        writeQueue_.pop_front();
      }
      writeChunkOffset_ = 0;
    }
    // 从偏移处继续发送，避免每次对 writeChunk_ 整体 erase(0,n) 的 O(n) 前移
    const char* data = writeChunk_.data() + writeChunkOffset_;
    size_t remain = writeChunk_.size() - writeChunkOffset_;
    ssize_t n = ::send(fd_, data, remain, MSG_NOSIGNAL);
    if (n > 0) {
      // 发送成功 n 字节，从权威计数中扣减
      writeBufferedBytes_.fetch_sub(static_cast<size_t>(n), std::memory_order_relaxed);
      writeChunkOffset_ += static_cast<size_t>(n);
      if (writeChunkOffset_ >= writeChunk_.size()) {
        writeChunk_.clear();
        writeChunkOffset_ = 0;
      }
    } else if (n == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // 对端暂时不可写：登记 EPOLLOUT，可写时由 write 回调继续 flush
        channel_.enableWriting(loop_);
        return;
      }
      if (errno == EINTR) continue;
      LOG_E("[conn] fd=" << fd_ << " send 错误: " << std::strerror(errno));
      closeInLoop();
      return;
    } else {
      closeInLoop();
      return;
    }
  }
  // 全部写完，撤销 EPOLLOUT 关注
  if (channel_.isWriting()) channel_.disableWriting(loop_);
  // 优雅关闭请求已排队：写完即关（P0-3）
  if (closingAfterFlush_) closeInLoop();
}

void Connection::handleRead() {
  if (closed_.load()) return;
  char buf[kReadBufferSize];
  size_t total = 0;
  for (;;) {
    ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
    if (n > 0) {
      inBuffer_.append(buf, static_cast<size_t>(n));
      total += static_cast<size_t>(n);
      // 单次读事件累计上限：ET 下一次洪泛可能瞬间灌入海量数据，
      // 先计入 inBuffer_ 再交付上层，若不加限制会导致瞬时大内存分配。
      // 超过上限视为恶意洪泛，直接断开。
      if (total > kMaxReadPerEvent) {
        LOG_E("[conn] fd=" << fd_ << " 单次读事件超过 "
              << kMaxReadPerEvent << " 字节，断开洪泛连接");
        closeInLoop();
        return;
      }
    } else if (n == 0) {
      closeInLoop(); // 对端关闭
      return;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break; // ET：读到 EAGAIN 才算读完
      if (errno == EINTR) continue;
      handleError();
      return;
    }
  }
  if (!inBuffer_.empty()) {
    // ET 一次性读完，交给上层解析（零拷贝：move 语义，避免二次拷贝）
    auto self = shared_from_this();
    if (messageCb_) messageCb_(self, std::move(inBuffer_));
  }
}

void Connection::handleError() {
  if (closed_.load()) return;
  LOG_E("[conn] fd=" << fd_ << " 异常/对端断开");
  closeInLoop();
}

void Connection::shutdown() {
  auto self = shared_from_this();
  loop_->runInLoop([self] { self->closeInLoop(); });
}

void Connection::shutdownAfterFlush() {
  auto self = shared_from_this();
  loop_->runInLoop([self] {
    if (self->closed_.load()) return;
    self->closingAfterFlush_ = true;
    // 关闭 deadline：防慢客户端写缓冲迟迟无法清空导致连接占用（P0-3）
    if (self->closeTimer_ == 0) {
      self->closeTimer_ = self->loop_->addTimer(
          [self] { self->closeInLoop(); }, config::kCloseFlushDeadline);
    }
    self->flushWrite(); // 立即尝试清空剩余缓冲
  });
}

void Connection::forceClose() {
  closeInLoop();
}

void Connection::closeInLoop() {
  if (closed_.exchange(true)) return;
  if (closeTimer_ != 0) {
    loop_->cancelTimer(closeTimer_);
    closeTimer_ = 0;
  }
  loop_->remove(&channel_);
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  auto self = shared_from_this();
  if (closeCb_) closeCb_(self);
}

} // namespace im