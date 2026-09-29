// ============================================================
// Channel：一个 fd 的事件关注与回调分发（epoll 的封装）
// 只记录"关注什么 + 回调是什么"，实际 epoll_ctl 由 EventLoop 执行。
// enable/disable Reading/Writing 系列须在所属 loop 线程调用。
// ============================================================
#include "channel.hpp"

#include "eventloop.hpp"

#include <sys/epoll.h>

namespace im {

Channel::Channel(int fd) : fd_(fd) {}

void Channel::enableReading(EventLoop* loop) {
  reading_ = true;
  rearm(loop);
}

void Channel::disableReading(EventLoop* loop) {
  reading_ = false;
  rearm(loop);
}

void Channel::enableWriting(EventLoop* loop) {
  writing_ = true;
  rearm(loop);
}

void Channel::disableWriting(EventLoop* loop) {
  writing_ = false;
  rearm(loop);
}

void Channel::rearm(EventLoop* loop) {
  if (loop) loop->update(this);
}

void Channel::handleEvent(uint32_t events) {
  // 先复制回调再执行，避免回调销毁本 channel（含自移除）后继续访问成员
  auto closeCb = closeCb_;
  auto readCb = readCb_;
  auto writeCb = writeCb_;
  if (events & (EPOLLERR | EPOLLHUP)) {
    if (closeCb) closeCb();
    return;
  }
  if (events & (EPOLLIN | EPOLLRDHUP)) {
    if (readCb) readCb();
  }
  // 注意：写回调用我们通过 addPendingFlush 驱动，不直接在 EPOLLOUT 里
  // 处理，避免事件顺序问题。此处仍保留 writeCb_ 以备特殊场景。
  if (events & EPOLLOUT) {
    if (writeCb) writeCb();
  }
}

} // namespace im