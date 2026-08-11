#include "connection.hpp"

#include "eventloop.hpp"
#include "util.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace im {

namespace {
constexpr size_t kReadBufferSize = 64 * 1024;
// 单次读事件累计上限：防 ET 洪泛导致 inBuffer_ 瞬时大内存分配
constexpr size_t kMaxReadPerEvent = 1 * 1024 * 1024;
// 写缓冲上限：超过则视为慢消费/不可达连接，主动断开避免内存无限增长
constexpr size_t kMaxWriteBufferBytes = 8 * 1024 * 1024;
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

void Connection::send(const char* data, size_t len) {
  if (closed_.load() || queuedTooMuch_.load()) return;
  // 先原子判断是否超限。writeBufferedBytes_ 是唯一权威的字节计数，
  // 不再读取 writeChunk_（其仅由 loop 线程修改），消除跨线程数据竞争。
  size_t prev = writeBufferedBytes_.fetch_add(len, std::memory_order_relaxed);
  if (prev + len > kMaxWriteBufferBytes) {
    // 超限：回滚计数并标记，避免重复触发断开
    writeBufferedBytes_.fetch_sub(len, std::memory_order_relaxed);
    queuedTooMuch_.store(true, std::memory_order_relaxed);
    std::cerr << "[conn] fd=" << fd_ << " 写缓冲超限，断开慢消费/不可达连接" << std::endl;
    shutdown();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(writeMutex_);
    writeQueue_.emplace_back(data, len);
  }
  auto self = shared_from_this();
  loop_->addPendingFlush(std::move(self));
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
      std::cerr << "[conn] fd=" << fd_ << " send 错误: " << std::strerror(errno) << std::endl;
      closeInLoop();
      return;
    } else {
      closeInLoop();
      return;
    }
  }
  // 全部写完，撤销 EPOLLOUT 关注
  if (channel_.isWriting()) channel_.disableWriting(loop_);
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
        std::cerr << "[conn] fd=" << fd_ << " 单次读事件超过 "
                  << kMaxReadPerEvent << " 字节，断开洪泛连接" << std::endl;
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
  std::cerr << "[conn] fd=" << fd_ << " 异常/对端断开" << std::endl;
  closeInLoop();
}

void Connection::shutdown() {
  auto self = shared_from_this();
  loop_->runInLoop([self] { self->closeInLoop(); });
}

void Connection::forceClose() {
  closeInLoop();
}

void Connection::closeInLoop() {
  if (closed_.exchange(true)) return;
  loop_->remove(&channel_);
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  auto self = shared_from_this();
  if (closeCb_) closeCb_(self);
}

} // namespace im