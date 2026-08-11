#pragma once

#include <cstdint>
#include <functional>

namespace im {

class EventLoop;

// 一个 fd 的事件关注对象。回调仅在 EventLoop::run() 所在线程触发。
class Channel {
public:
  explicit Channel(int fd);

  void setReadCallback(std::function<void()> cb) { readCb_ = std::move(cb); }
  void setWriteCallback(std::function<void()> cb) { writeCb_ = std::move(cb); }
  void setCloseCallback(std::function<void()> cb) { closeCb_ = std::move(cb); }

  void enableReading(EventLoop* loop);
  void disableReading(EventLoop* loop);
  void enableWriting(EventLoop* loop);
  void disableWriting(EventLoop* loop);

  void handleEvent(uint32_t events);

  int fd() const { return fd_; }
  void setFd(int fd) { fd_ = fd; }
  bool isReading() const { return reading_; }
  bool isWriting() const { return writing_; }

private:
  void rearm(EventLoop* loop);

  int fd_;
  bool reading_ = false;
  bool writing_ = false;
  std::function<void()> readCb_;
  std::function<void()> writeCb_;
  std::function<void()> closeCb_;
};

} // namespace im