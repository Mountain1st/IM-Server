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
    if (events & (EPOLLERR | EPOLLHUP)) {
        if (closeCb_) closeCb_();
        return;
    }
    if (events & (EPOLLIN | EPOLLRDHUP)) {
        if (readCb_) readCb_();
    }
    // 注意：写回调用我们通过 addPendingFlush 驱动，不直接在 EPOLLOUT 里
    // 处理，避免事件顺序问题。此处仍保留 writeCb_ 以备特殊场景。
    if (events & EPOLLOUT) {
        if (writeCb_) writeCb_();
    }
}

} // namespace im