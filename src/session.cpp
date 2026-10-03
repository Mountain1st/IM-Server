// ============================================================
// Session：一条 WebSocket 连接上的应用层会话（握手 → 帧 → 业务）
// - 握手：handleHandshake（严格校验请求行/Host/Key/Origin 白名单，
//   10 秒超时）；/health HTTP 短路应答（不升级 WS）
// - 升级后：processFrames 解析 WS 帧（分片重组/控制帧处理）；
//   Ping 自动回 Pong（保活由协议层承担）；Close 回镜像帧后写完再关
// - 文本帧：经 ThreadPool 投递到工作线程（enqueueAndDrain，8MB 队列
//   上限防慢消费堆积——超限断开），业务层再回推（reply 直接 send）
// - 关闭：onClose 取消定时器 → 通知 AppServer 摘在线表 → 释放自持
// 线程：帧解析/心跳定时器在 IO loop 线程；文本业务在工作线程池
// ============================================================
#include "session.hpp"

#include "connection.hpp"
#include "eventloop.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "websocket.hpp"
#include "server_config.hpp"
#include "threadpool.hpp"

#include <iostream>
#include <sstream>

namespace im {

// 心跳/超时/队列/消息限制统一取自集中配置（server_config.hpp）
namespace {

constexpr std::chrono::seconds kHeartbeatInterval(config::kHeartbeatInterval);
constexpr std::chrono::seconds kZombieTimeout(config::kZombieTimeout);
constexpr std::chrono::seconds kHandshakeTimeout(config::kHandshakeTimeout);
constexpr size_t kMaxQueuedBytes = config::kMaxQueuedBytes;
constexpr size_t kMaxMessageBytes = config::kMaxMessageBytes;
}

Session::Session(EventLoop* loop, ThreadPool* pool,
        const std::shared_ptr<Connection>& conn)
  : loop_(loop), pool_(pool), conn_(conn), fd_(conn->fd()) {}

Session::Session(EventLoop* loop, ThreadPool* pool,
        const std::shared_ptr<Connection>& conn,
        std::vector<std::string> allowedOrigins)
  : loop_(loop), pool_(pool), conn_(conn), fd_(conn->fd()),
    allowedOrigins_(std::move(allowedOrigins)) {}

Session::Session(EventLoop* loop, ThreadPool* pool,
        const std::shared_ptr<Connection>& conn,
        std::vector<std::string> allowedOrigins,
        std::vector<std::string> trustedProxies)
  : loop_(loop), pool_(pool), conn_(conn), fd_(conn->fd()),
    allowedOrigins_(std::move(allowedOrigins)),
    trustedProxies_(std::move(trustedProxies)) {}

Session::~Session() {
  LOG_I("[session] 会话销毁");
}

void Session::start() {
  // 连接可能在被调度到本函数前已由 loop 线程关闭（如先到达的关闭任务）。
  // 此时 fd 已置 -1，不能对已关闭 fd 注册 epoll；直接走关闭清理路径，
  // 由上层摘除在线表并移除连接，避免残留无效会话。
  if (conn_->isClosed()) {
    LOG_E("[session] fd=" << fd_ << " start 时连接已关闭，直接清理");
    if (closedHandler_) closedHandler_();
    return;
  }
  // 自持，保证收到关闭前本会话一定存活；Connection 侧只持有弱引用
  guard_ = shared_from_this();
  parser_.setMaxBytes(kMaxMessageBytes);
  auto weakSelf = std::weak_ptr<Session>(guard_);
  conn_->setMessageCallback(
    [weakSelf](const auto& conn, std::string data) {
      auto self = weakSelf.lock();
      if (!self) return;
      // 读事件在 loop 线程发生。任何收到数据即视为连接存活
      self->lastRecv_ = std::chrono::steady_clock::now();
      // 把数据按序入队，由线程池串行处理（move 语义，零拷贝）
      self->enqueueAndDrain(conn, std::move(data));
    });
  conn_->setCloseCallback(
    [weakSelf](const std::shared_ptr<Connection>& conn) {
      auto self = weakSelf.lock();
      if (!self) return;
      self->onClose(conn);
    });
  conn_->start();

  lastRecv_ = std::chrono::steady_clock::now();
  // 在会话所属的 IO loop 线程注册定时器：握手截止 + 心跳
  loop_->runInLoop([weakSelf, this] {
    if (!guard_) return; // 已被关闭
    auto self = weakSelf.lock();
    if (!self) return;
    if (self->handshakeTimer_ == 0) {
      // 周期触发，回调内判定：超时仍未握手则断开
      self->handshakeTimer_ =
        loop_->addTimer([weakSelf] {
          auto s = weakSelf.lock();
          if (!s) return;
          if (!s->handshaked_) {
            LOG_I("[session] fd=" << s->fd_
                  << " 握手超时（" << kHandshakeTimeout.count() << "s），断开");
            s->conn_->shutdown();
          }
        }, kHandshakeTimeout);
    }
    if (self->heartbeatTimer_ == 0) {
      self->heartbeatTimer_ =
        loop_->addTimer([weakSelf] {
          auto s = weakSelf.lock();
          if (s) s->onHeartbeat();
        }, kHeartbeatInterval);
    }
  });
}

void Session::onHeartbeat() {
  auto now = std::chrono::steady_clock::now();

  // 僵尸判定：超过 kZombieTimeout 没收到过任何数据/Pong，直接断开
  if (now - lastRecv_ > kZombieTimeout) {
    LOG_I("[session] fd=" << fd_ << " 心跳超时，判定为僵尸连接，断开");
    conn_->shutdown();
    return;
  }

  // 到点发一个 Ping 探活
  if (now - lastPingSent_ >= kHeartbeatInterval) {
    lastPingSent_ = now;
    conn_->send(ws::buildFrame(ws::Opcode::Ping, "heartbeat"));
  }
}

void Session::enqueueAndDrain(const std::shared_ptr<Connection>&, std::string data) {
  bool needTask = false;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (queueOverflow_) return; // 已超限，丢弃后续数据
    if (queuedBytes_ + data.size() > kMaxQueuedBytes) {
      // 读方向洪泛：清空队列并断开，避免等待积压继续增长
      queueOverflow_ = true;
      pendingData_.clear();
      queuedBytes_ = 0;
    } else {
      pendingData_.emplace_back(std::move(data));
      queuedBytes_ += pendingData_.back().size();
      if (!draining_) {
        draining_ = true;
        needTask = true;
      }
    }
  }
  if (queueOverflow_) {
    LOG_E("[session] fd=" << fd_ << " 读队列超限，断开洪泛连接");
    conn_->shutdown();
    return;
  }
  if (needTask) {
    auto self = shared_from_this();
    pool_->submit([self] { self->drain(); });
  }
}

void Session::drain() {
  for (;;) {
    std::string chunk;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (pendingData_.empty()) {
        draining_ = false;
        return;
      }
      chunk = std::move(pendingData_.front());
      pendingData_.pop_front();
      queuedBytes_ -= chunk.size();
    }
    // 业务回调可能抛出异常（如 JSON 字段类型错误）。若不在工作线程兜底，
    // 异常会逃逸到线程池 worker 导致 std::terminate 整个进程崩溃，故捕获并断开该连接。
    try {
      onMessage(conn_, chunk.data(), chunk.size());
    } catch (const std::exception& e) {
      LOG_E("[session] fd=" << fd_ << " 业务处理异常: " << e.what());
      conn_->shutdown();
      return;
    } catch (...) {
      LOG_E("[session] fd=" << fd_ << " 未知业务异常，断开");
      conn_->shutdown();
      return;
    }
  }
}

void Session::onMessage(const std::shared_ptr<Connection>&, const char* data, size_t len) {
  if (parser_.overflow()) {
    LOG_E("[session] fd=" << fd_ << " 帧缓冲超限，断开");
    conn_->shutdown();
    return;
  }
  if (!handshaked_) {
    // 累积 HTTP 握手头
    httpBuffer_.append(data, len);
    size_t end = httpBuffer_.find("\r\n\r\n");
    if (end == std::string::npos) {
      if (httpBuffer_.size() > config::kMaxHandshakeHeaderBytes) {
        LOG_E("[session] 握手头过大");
        conn_->shutdown();
      }
      return;
    }
    std::string request = httpBuffer_.substr(0, end + 4);
    // /health 健康检查（非 WebSocket）：返回指标 JSON 后关闭
    if (request.rfind("GET /health", 0) == 0) {
      serveHealth();
      return;
    }
    handleHandshake(request);
    if (!handshaked_) return; // 握手失败已关闭

    std::string rest = httpBuffer_.substr(end + 4);
    httpBuffer_.clear();
    if (!rest.empty()) parser_.feed(rest.data(), rest.size());
    if (parser_.overflow()) {
      LOG_E("[session] fd=" << fd_ << " 帧缓冲超限，断开");
      conn_->shutdown();
      return;
    }
    processFrames();
    return;
  }
  parser_.feed(data, len);
  processFrames();
}

void Session::processFrames() {
  ws::Frame frame;
  for (;;) {
    ws::ParseResult pr = parser_.parseOne(&frame);
    if (pr == ws::ParseResult::NeedMore) return;
    if (pr == ws::ParseResult::ProtocolError) {
      g_metrics.protocolErrors.fetch_add(1, std::memory_order_relaxed);
      LOG_E("[session] fd=" << fd_ << " 协议错误，断开");
      conn_->shutdown();
      return;
    }
    handle(frame);
  }
}

void Session::rejectHandshake(const std::string& reason) {
  g_metrics.handshakeFailures.fetch_add(1, std::memory_order_relaxed);
  LOG_E("[session] 握手拒绝: " << reason);
  conn_->send("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  // 写完 403 再关闭（优雅关闭，带 deadline 兜底），避免拒绝响应被丢弃
  conn_->shutdownAfterFlush();
}

void Session::serveHealth() {
  std::ostringstream oss;
  oss << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n";
  oss << "{\"status\":\"ok\""
      << ",\"activeConnections\":" << g_metrics.activeConnections.load(std::memory_order_relaxed)
      << ",\"connectionsAccepted\":" << g_metrics.connectionsAccepted.load(std::memory_order_relaxed)
      << ",\"connectionsClosed\":" << g_metrics.connectionsClosed.load(std::memory_order_relaxed)
      << ",\"handshakeFailures\":" << g_metrics.handshakeFailures.load(std::memory_order_relaxed)
      << ",\"protocolErrors\":" << g_metrics.protocolErrors.load(std::memory_order_relaxed)
      << ",\"authFailures\":" << g_metrics.authFailures.load(std::memory_order_relaxed)
      << ",\"rateLimited\":" << g_metrics.rateLimited.load(std::memory_order_relaxed)
      << ",\"messagesDelivered\":" << g_metrics.messagesDelivered.load(std::memory_order_relaxed)
      << ",\"messagesOffline\":" << g_metrics.messagesOffline.load(std::memory_order_relaxed)
      << ",\"writeOverflows\":" << g_metrics.writeOverflows.load(std::memory_order_relaxed)
      << ",\"mediaCommitRejected\":" << g_metrics.mediaCommitRejected.load(std::memory_order_relaxed)
      << "}\r\n";
  conn_->send(oss.str());
  conn_->shutdownAfterFlush();
}

void Session::handleHandshake(const std::string& request) {
  std::string key;
  std::string reason;
  if (!ws::validateHandshake(request, &key, &reason)) {
    g_metrics.handshakeFailures.fetch_add(1, std::memory_order_relaxed);
    LOG_E("[session] 握手校验失败: " << reason);
    conn_->shutdown();
    return;
  }

  // Origin/Host 校验（防跨站 WebSocket 连接 / Host 头注入）。
  // 配置了允许列表才启用；缺省不校验，兼容命令行工具等不携带 Origin 的客户端。
  if (!allowedOrigins_.empty()) {
    const std::string origin = ws::getRequestHeader(request, "Origin");
    const std::string host = ws::getRequestHeader(request, "Host");
    if (!origin.empty() && !ws::originAllowed(origin, allowedOrigins_)) {
      rejectHandshake("Origin 不在允许列表 (" + origin + ")");
      return;
    }
    if (!host.empty() && !ws::hostAllowed(host, allowedOrigins_)) {
      rejectHandshake("Host 不在允许列表 (" + host + ")");
      return;
    }
  }

  std::string accept = ws::computeAccept(key);

  std::ostringstream oss;
  oss << "HTTP/1.1 101 Switching Protocols\r\n"
    << "Upgrade: websocket\r\n"
    << "Connection: Upgrade\r\n"
    << "Sec-WebSocket-Accept: " << accept << "\r\n"
    << "\r\n";
  conn_->send(oss.str());
  handshaked_ = true;
  // 真实 IP（Nginx 反代场景）：对端属可信代理时取 X-Forwarded-For 首段。
  // 未配置可信代理则忽略头（默认行为不变，防客户端伪造）。
  if (!trustedProxies_.empty()) {
    for (const auto& p : trustedProxies_) {
      if (p == conn_->peerIp()) {
        realIp_ = ws::firstForwardedIp(request);
        break;
      }
    }
  }
  // 握手完成，取消握手截止定时器（跨线程安全，投递到 loop 执行）
  if (handshakeTimer_ != 0) {
    loop_->cancelTimer(handshakeTimer_);
    handshakeTimer_ = 0;
  }
  LOG_I("[session] WebSocket 握手成功"
        << (realIp_.empty() ? "" : " 真实IP=" + realIp_));
}

void Session::handle(const ws::Frame& frame) {
  // 控制帧（Close/Ping/Pong）校验：payload 必须 ≤125 字节且不可分片（RFC 6455 §5.5）。
  // 缺失校验会让恶意客户端用大 Ping 触发同量级 Pong 回发，造成内存/带宽放大。
  if (frame.opcode >= ws::Opcode::Close && (frame.payload.size() > 125 || !frame.fin)) {
    LOG_E("[session] fd=" << fd_ << " 非法控制帧，断开");
    conn_->shutdown();
    return;
  }
  switch (frame.opcode) {
  case ws::Opcode::Text:
  case ws::Opcode::Binary:
    // 已有未完成分片时再收新数据帧：RFC 6455 要求协议错误断开，禁止静默覆盖旧分片
    if (fragmenting_) {
      LOG_E("[session] fd=" << fd_ << " 分片未完成收到新数据帧，断开");
      conn_->shutdown();
      return;
    }
    if (frame.fin) {
      if (frame.payload.size() > kMaxMessageBytes) {
        LOG_E("[session] fd=" << fd_ << " 单条消息过大");
        conn_->shutdown();
        return;
      }
      handleData(frame.opcode, frame.payload);
    } else {
      fragmenting_ = true;
      fragmentOpcode_ = frame.opcode;
      fragmentBuffer_ = frame.payload;
      if (fragmentBuffer_.size() > kMaxMessageBytes) {
        LOG_E("[session] fd=" << fd_ << " 分片累积超限");
        conn_->shutdown();
        return;
      }
    }
    break;
  case ws::Opcode::Continuation:
    if (!fragmenting_) {
      LOG_E("[session] 收到意外分片帧");
      conn_->shutdown();
      return;
    }
    fragmentBuffer_ += frame.payload;
    if (fragmentBuffer_.size() > kMaxMessageBytes) {
      LOG_E("[session] fd=" << fd_ << " 分片累积超限");
      conn_->shutdown();
      return;
    }
    if (frame.fin) {
      handleData(fragmentOpcode_, fragmentBuffer_);
      fragmenting_ = false;
      fragmentBuffer_.clear();
    }
    break;
  case ws::Opcode::Ping:
    // 自动回 Pong
    conn_->send(ws::buildFrame(ws::Opcode::Pong, frame.payload));
    break;
  case ws::Opcode::Pong:
    break;
  case ws::Opcode::Close:
    // 回 Close 帧后写完再关闭（优雅，P0-3）
    conn_->send(ws::buildFrame(ws::Opcode::Close, frame.payload));
    conn_->shutdownAfterFlush();
    break;
  default:
    conn_->shutdown();
    break;
  }
}

void Session::handleData(ws::Opcode opcode, const std::string& payload) {
  if (opcode == ws::Opcode::Text) {
    // RFC 6455 §8.1：文本消息必须是合法 UTF-8；非法则协议错误断开
    if (!ws::isValidUtf8(payload)) {
      LOG_E("[session] fd=" << fd_ << " 文本消息非法 UTF-8，断开");
      conn_->shutdown();
      return;
    }
    // 只记录字节数，不打印消息内容（隐私/安全脱敏）
    LOG_I("[session] fd=" << fd_ << " 收到文本消息 " << payload.size()
          << " 字节");
    // 交由业务层处理（AppServer 通过 textHandler_ 接入回显等业务）
    if (textHandler_) textHandler_(shared_from_this(), payload);
    return;
  }
  // 二进制等其它数据仅记录字节数
  LOG_I("[session] fd=" << fd_
        << " 收到二进制数据 " << payload.size() << " 字节");
}

void Session::reply(const std::string& text) {
  conn_->send(ws::buildFrame(ws::Opcode::Text, text));
}

void Session::onClose(const std::shared_ptr<Connection>&) {
  // 取消心跳与握手截止定时器
  if (heartbeatTimer_ != 0) {
    loop_->cancelTimer(heartbeatTimer_);
    heartbeatTimer_ = 0;
  }
  if (handshakeTimer_ != 0) {
    loop_->cancelTimer(handshakeTimer_);
    handshakeTimer_ = 0;
  }
  LOG_I("[session] fd=" << fd_ << " 断开连接");
  if (closedHandler_) closedHandler_();
  // 释放自持，让会话在本函数结束后可被销毁（不再循环引用）
  guard_.reset();
}

} // namespace im