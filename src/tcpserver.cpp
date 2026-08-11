#include "tcpserver.hpp"

#include "connection.hpp"
#include "eventloop_threadpool.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>

namespace im {

TcpServer::TcpServer(uint16_t port, size_t ioThreads)
    : TcpServer(port, ioThreads, 0) {}

TcpServer::TcpServer(uint16_t port, size_t ioThreads, size_t maxConnections)
    : ioPool_(std::make_unique<EventLoopThreadPool>(ioThreads)),
      listenFd_(-1), acceptChannel_(-1), maxConnections_(maxConnections) {
  std::cout << "[server] 配置: 端口 " << port
            << ", 从 eventloop 线程 " << ioThreads << std::endl;
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
  // 先启动从 eventloop 线程池
  ioPool_->start();
  // 主 eventloop 注册监听 fd；主 loop 由 run() 在同一线程运行
  acceptChannel_.enableReading(&acceptLoop_);
}

void TcpServer::run() {
  acceptLoop_.run();
}

void TcpServer::requestStop() {
  // 只做两件事：让主 loop 退出（running_=false）+ 写 eventfd 唤醒
  // epoll_wait。均为 async-signal-safe，可在信号处理器中安全调用。
  acceptLoop_.stop();
}

void TcpServer::stop() {
  // 1) 主 loop 停止接受新连接
  acceptLoop_.stop();
  // 2) 关闭所有现存连接。
  //    - 归属主 loop（即主线程）的连接：直接同步关闭；
  //    - 归属从 loop 的连接：在各自 loop 线程投递「关闭全部 + barrier」。
  //    主线程等待所有 barrier 完成后才停止从 loop，保证关闭任务必然在
  //    loop 存活期间执行——否则 stop 竞态会让已投递任务被跳过，导致连接/
  //    会话（含上层路由清理）不关闭。
  std::vector<ConnectionPtr> conns;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    conns.assign(connections_.begin(), connections_.end());
  }
  std::map<EventLoop*, std::vector<ConnectionPtr>> byLoop;
  for (auto& conn : conns) {
    EventLoop* loop = conn->loop();
    if (loop == &acceptLoop_) {
      conn->forceClose();
    } else {
      byLoop[loop].push_back(conn);
    }
  }

  struct Barrier {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
  };
  std::vector<std::shared_ptr<Barrier>> barriers;
  barriers.reserve(byLoop.size());
  for (auto& [loop, list] : byLoop) {
    auto bar = std::make_shared<Barrier>();
    barriers.push_back(bar);
    loop->runInLoop([list, bar] {
      for (auto& c : list) c->forceClose();
      {
        std::lock_guard<std::mutex> lock(bar->m);
        bar->done = true;
      }
      bar->cv.notify_all();
    });
  }
  for (auto& bar : barriers) {
    std::unique_lock<std::mutex> lock(bar->m);
    bar->cv.wait(lock, [&bar] { return bar->done; });
  }

  // 3) 全部连接已关闭，从 loop 直接退出
  ioPool_->stop();
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
  // 连接数上限检查：超限则立即拒绝并关闭（防连接耗尽）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (maxConnections_ > 0 && connections_.size() >= maxConnections_) {
      std::cerr << "[server] 连接数已达上限 (" << maxConnections_
                << ")，拒绝 fd=" << cfd << std::endl;
      ::close(cfd);
      return;
    }
  }
  // 简单分配：客户端 fd 对从线程数量求模，余数决定由哪个从 loop 处理
  size_t n = ioPool_->size();
  EventLoop* ioLoop = (n > 0) ? ioPool_->getLoop(static_cast<size_t>(cfd) % n) : nullptr;
  if (!ioLoop) ioLoop = &acceptLoop_;
  auto conn = std::make_shared<Connection>(ioLoop, cfd);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.insert(conn);
  }
  auto self = shared_from_this();
  ioLoop->runInLoop([this, self, conn, ioLoop] {
    if (newConnCb_) newConnCb_(ioLoop, conn);
    else conn->start();
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