// ============================================================
// TcpServer：网络层服务器（主从 reactor）
// - 主 loop（acceptLoop_）：只做 accept（IPv6+IPv4 双 socket 监听，
//   兼容 localhost 双栈解析）
// - 从 loop 池（ioPool_）：各连接的实际读写；新连接按最少连接优先
//   分配（per-loop 原子计数，替代 fd % N 防倾斜）
// - 连接治理：总上限/单 IP 上限（超限 accept 即拒并 close）
// - 停机：requestStop（async-signal-safe 唤醒主 loop）→
//   gracefulShutdown（对在线连接发 Close(1001) 后写完即关，barrier
//   等下发完成）→ stop（残余 forceClose + 停从 loop，barrier 防竞态）
// 线程：accept 在主 loop；连接回调在各自从 loop；本类互斥锁保护
// 连接表/计数
// ============================================================
#include "tcpserver.hpp"

#include "connection.hpp"
#include "eventloop_threadpool.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "util.hpp"
#include "websocket.hpp"

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

TcpServer::TcpServer(uint16_t port, size_t ioThreads, size_t maxConnections,
                     size_t maxPerIpConnections)
    : ioPool_(std::make_unique<EventLoopThreadPool>(ioThreads)),
      maxConnections_(maxConnections),
      maxPerIpConnections_(maxPerIpConnections),
      loopCounts_(ioThreads) { // n 格原子计数（vector<atomic> 只能构造期定长，reserve 会实例化搬迁路径）
  LOG_I("[server] 配置: 端口 " << port
        << ", 从 eventloop 线程 " << ioThreads);
  // 同时监听 IPv6 与 IPv4：WSL2 的 localhost 中继会按服务器监听的地址族
  // 在 Windows 侧绑定对应的环回监听（127.0.0.1 或 ::1）。只绑单一地址族时，
  // 客户端用另一地址族的主机名/地址会连不上，故双 socket 全覆盖。
  bool v6 = addListener(AF_INET6, /*ipv6Only=*/true, port, "IPv6 [::]");
  bool v4 = addListener(AF_INET, /*ipv6Only=*/false, port, "IPv4 0.0.0.0");
  if (!v6 && !v4) {
    LOG_E("[server] 无法监听端口 " << port);
    ::exit(1);
  }
  LOG_I("[server] 监听端口 " << port << " ("
        << (v6 ? "IPv6" : "") << (v6 && v4 ? " + " : "")
        << (v4 ? "IPv4" : "") << ")");
}

bool TcpServer::addListener(int family, bool ipv6Only, uint16_t port,
                            const std::string& label) {
  int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    LOG_E("[server] " << label << " socket 失败: "
          << std::strerror(errno));
    return false;
  }
  setReuseAddr(fd);
  if (family == AF_INET6) {
    int on = ipv6Only ? 1 : 0;
    if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on) < 0) {
      LOG_E("[server] " << label << " 设置 IPV6_V6ONLY 失败: "
            << std::strerror(errno));
    }
  }
  if (family == AF_INET6) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
      LOG_E("[server] " << label << " bind " << port << " 失败: "
            << std::strerror(errno));
      ::close(fd);
      return false;
    }
  } else {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
      LOG_E("[server] " << label << " bind " << port << " 失败: "
            << std::strerror(errno));
      ::close(fd);
      return false;
    }
  }
  if (::listen(fd, SOMAXCONN) < 0) {
    LOG_E("[server] " << label << " listen 失败: "
          << std::strerror(errno));
    ::close(fd);
    return false;
  }
  Listener li;
  li.fd = fd;
  li.channel.setFd(fd);
  li.channel.setReadCallback([this] { handleAccept(); });
  li.channel.setCloseCallback([this] { handleAccept(); });
  listeners_.push_back(std::move(li));
  return true;
}

TcpServer::~TcpServer() {
  for (auto& li : listeners_)
    if (li.fd >= 0) ::close(li.fd);
}

void TcpServer::start() {
  // 先启动从 eventloop 线程池
  ioPool_->start();
  // 建立 loop -> 下标 映射（此后只读）：连接关闭时按归属扣减 per-loop 计数
  const size_t n = ioPool_->size();
  for (size_t i = 0; i < n; ++i) {
    EventLoop* loop = ioPool_->getLoop(i);
    if (loop) loopIndex_[loop] = i;
  }
  // 主 eventloop 注册各监听 fd；主 loop 由 run() 在同一线程运行
  for (auto& li : listeners_) {
    li.channel.enableReading(&acceptLoop_);
  }
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

void TcpServer::gracefulShutdown() {
  std::vector<ConnectionPtr> conns;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    conns.assign(connections_.begin(), connections_.end());
  }
  if (conns.empty()) return;
  // WS Close 帧载荷：状态码 1001 Going Away（2 字节网络序）
  const std::string closeFrame =
      ws::buildFrame(ws::Opcode::Close, std::string("\x03\xe9", 2));
  // 按 loop 归类：从 loop 的连接投递到其线程（发帧 → 写完即关），
  // 主 loop 的连接直接关闭（主 loop 已停，runInLoop 不会执行）
  std::map<EventLoop*, std::vector<ConnectionPtr>> byLoop;
  for (auto& conn : conns) {
    if (conn->loop() == &acceptLoop_) {
      conn->forceClose();
    } else {
      byLoop[conn->loop()].push_back(conn);
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
    loop->runInLoop([list, bar, closeFrame] {
      for (auto& c : list) {
        // send 线程安全；shutdownAfterFlush 立即尝试写出并登记
        // "写完即关"（含 flush 超时兜底定时器），IO loop 仍运行可完成
        c->send(closeFrame.data(), closeFrame.size());
        c->shutdownAfterFlush();
      }
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
  LOG_I("[server] 优雅停机：已向 " << conns.size() << " 个在线连接发送 Close(1001)");
}

void TcpServer::handleAccept() {
  for (auto& li : listeners_) {
    for (;;) {
      sockaddr_storage addr{};
      socklen_t addrLen = sizeof addr;
      int cfd = ::accept(li.fd, reinterpret_cast<sockaddr*>(&addr), &addrLen);
      if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // ET：accept 到 EAGAIN
        if (errno == EINTR) continue;
        break;
      }
      setNoDelay(cfd);
      setNonBlocking(cfd);

      char ip[INET6_ADDRSTRLEN] = {0};
      uint16_t peerPort = 0;
      if (addr.ss_family == AF_INET6) {
        auto* a6 = reinterpret_cast<sockaddr_in6*>(&addr);
        // IPv4-mapped 地址（::ffff:x.x.x.x）显示为 IPv4 形式
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
          ::inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], ip, sizeof ip);
        } else {
          ::inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof ip);
        }
        peerPort = ntohs(a6->sin6_port);
      } else {
        auto* a4 = reinterpret_cast<sockaddr_in*>(&addr);
        ::inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof ip);
        peerPort = ntohs(a4->sin_port);
      }
      LOG_I("[server] 新连接 " << ip << ":" << peerPort
            << " fd=" << cfd);

      g_metrics.connectionsAccepted.fetch_add(1, std::memory_order_relaxed);
      g_metrics.activeConnections.fetch_add(1, std::memory_order_relaxed);
      deviceNewConnection(cfd, ip);
    }
  }
}

void TcpServer::deviceNewConnection(int cfd, const std::string& peerIp) {
  // 连接数上限检查：超限则立即拒绝并关闭（防连接耗尽）
  // 单 IP 上限检查：同 IP 并发超限同样拒绝（防单 IP 占槽；0 表示不限）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (maxConnections_ > 0 && connections_.size() >= maxConnections_) {
      LOG_E("[server] 连接数已达上限 (" << maxConnections_
            << ")，拒绝 fd=" << cfd);
      ::close(cfd);
      return;
    }
    if (maxPerIpConnections_ > 0) {
      size_t n = perIpCounts_[peerIp];
      if (n >= maxPerIpConnections_) {
        LOG_E("[server] IP " << peerIp << " 连接数已达上限 ("
              << maxPerIpConnections_ << ")，拒绝 fd=" << cfd);
        ::close(cfd);
        return;
      }
      perIpCounts_[peerIp] = n + 1;
    }
  }
  // 最少连接优先：选当前连接数最少的从 loop（替代 fd % n——
  // fd 回绕不均与连接寿命差异会造成负载倾斜，least-connections 始终均衡）
  const size_t n = ioPool_->size();
  EventLoop* ioLoop = nullptr;
  if (n > 0) {
    const size_t idx = pickLeastLoadedIndex(loopCounts_);
    ioLoop = ioPool_->getLoop(idx);
    if (ioLoop) loopCounts_[idx].fetch_add(1, std::memory_order_relaxed);
  }
  if (!ioLoop) ioLoop = &acceptLoop_;
  auto conn = std::make_shared<Connection>(ioLoop, cfd);
  conn->setPeerIp(peerIp); // 供会话层做 X-Forwarded-For 真实 IP 判定
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.insert(conn);
    if (maxPerIpConnections_ > 0) connIp_[conn.get()] = peerIp;
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
    // 首次移除才扣减 per-loop 计数（重复调用幂等，防双扣）
    if (connections_.erase(conn) > 0) {
      auto li = loopIndex_.find(conn->loop());
      if (li != loopIndex_.end() && li->second < loopCounts_.size()) {
        loopCounts_[li->second].fetch_sub(1, std::memory_order_relaxed);
      }
    }
    // 扣减该连接所属 IP 的计数
    auto it = connIp_.find(conn.get());
    if (it != connIp_.end()) {
      auto jt = perIpCounts_.find(it->second);
      if (jt != perIpCounts_.end()) {
        if (jt->second <= 1) perIpCounts_.erase(jt);
        else --jt->second;
      }
      connIp_.erase(it);
    }
  }
  g_metrics.activeConnections.fetch_sub(1, std::memory_order_relaxed);
  g_metrics.connectionsClosed.fetch_add(1, std::memory_order_relaxed);
  if (connClosedCb_) connClosedCb_(conn);
}

} // namespace im