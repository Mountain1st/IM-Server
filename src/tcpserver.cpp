#include "tcpserver.hpp"

#include "connection.hpp"
#include "eventloop.hpp"
#include "eventloop_threadpool.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace im {

TcpServer::TcpServer(EventLoop* loop, uint16_t port, EventLoopThreadPool* ioPool)
    : loop_(loop), ioPool_(ioPool), listenFd_(-1), acceptChannel_(-1) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        std::cerr << "[server] socket 失败: " << std::strerror(errno) << std::endl;
        ::exit(1);
    }
    setReuseAddr(listenFd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::cerr << "[server] bind " << port << " 失败: " << std::strerror(errno) << std::endl;
        ::exit(1);
    }
    if (::listen(listenFd_, SOMAXCONN) < 0) {
        std::cerr << "[server] listen 失败: " << std::strerror(errno) << std::endl;
        ::exit(1);
    }

    acceptChannel_.setFd(listenFd_);
    acceptChannel_.setReadCallback([this] { handleAccept(); });
    acceptChannel_.setCloseCallback([this] { handleAccept(); });
    std::cout << "[server] 监听端口 " << port << std::endl;
}

TcpServer::~TcpServer() {
    if (listenFd_ >= 0) ::close(listenFd_);
}

void TcpServer::start() {
    loop_->runInLoop([this] { acceptChannel_.enableReading(loop_); });
}

void TcpServer::handleAccept() {
    for (;;) {
        sockaddr_in addr{};
        socklen_t addrLen = sizeof addr;
        int cfd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // ET：accept 到 EAGAIN
            if (errno == EINTR) continue;
            break;
        }
        setNoDelay(cfd);
        setNonBlocking(cfd);

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
        std::cout << "[server] 新连接 " << ip << ":" << ntohs(addr.sin_port)
                  << " fd=" << cfd << std::endl;

        deviceNewConnection(cfd);
    }
}

void TcpServer::deviceNewConnection(int cfd) {
    // 在监听线程选一个 IO loop，把连接交付到该 loop 线程注册
    EventLoop* ioLoop = ioPool_->getNextLoop();
    auto conn = std::make_shared<Connection>(ioLoop, cfd);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.insert(conn);
    }
    auto self = shared_from_this();
    ioLoop->runInLoop([this, self, conn, ioLoop] {
        if (newConnCb_) newConnCb_(ioLoop, conn);
    });
}

void TcpServer::removeConnection(const ConnectionPtr& conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(conn);
    }
    if (connClosedCb_) connClosedCb_(conn);
}

} // namespace im