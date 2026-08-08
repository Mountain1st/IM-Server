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
    channel_.enableReading(loop_);
}

void Connection::send(const char* data, size_t len) {
    if (closed_.load()) return;
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        writeQueue_.emplace_back(data, len);
    }
    loop_->addPendingFlush(this);
}

void Connection::flushWrite() {
    if (closed_.load()) return;

    for (;;) {
        if (writeChunk_.empty()) {
            {
                std::lock_guard<std::mutex> lock(writeMutex_);
                if (writeQueue_.empty()) break;
                writeChunk_ = std::move(writeQueue_.front());
                writeQueue_.pop_front();
            }
        }
        ssize_t n = ::send(fd_, writeChunk_.data(), writeChunk_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            writeChunk_.erase(0, static_cast<size_t>(n));
        } else if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            std::cerr << "[conn] send 错误: " << std::strerror(errno) << std::endl;
            closeInLoop();
            return;
        } else {
            closeInLoop();
            return;
        }
    }
}

void Connection::handleRead() {
    if (closed_.load()) return;
    char buf[kReadBufferSize];
    for (;;) {
        ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n > 0) {
            inBuffer_.append(buf, static_cast<size_t>(n));
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
        // ET 一次性读完，交给上层解析
        auto self = shared_from_this();
        std::string data = std::move(inBuffer_);
        inBuffer_.clear();
        if (messageCb_) messageCb_(self, data.data(), data.size());
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