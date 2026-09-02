#pragma once

#include "eventloop.hpp"
#include "websocket.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace im {

class EventLoop;
class ThreadPool;
class Connection;

// 一个 WebSocket 会话：负责握手、帧解包、分片重组和业务处理。
class Session : public std::enable_shared_from_this<Session> {
public:
  Session(EventLoop* loop, ThreadPool* pool,
      const std::shared_ptr<Connection>& conn);
  // allowedOrigins：允许的 Origin/Host 列表（条目格式见 websocket.hpp）。
  // 为空表示不校验（向后兼容）。
  Session(EventLoop* loop, ThreadPool* pool,
      const std::shared_ptr<Connection>& conn,
      std::vector<std::string> allowedOrigins);
  ~Session();

  void start();
  void setClosedHandler(std::function<void()> handler) { closedHandler_ = std::move(handler); }
  // 业务回调：一条完整文本消息（由上层 AppServer 设置）
  using TextHandler = std::function<void(const std::shared_ptr<Session>&, const std::string&)>;
  void setTextHandler(TextHandler h) { textHandler_ = std::move(h); }
  void onHeartbeat();

  EventLoop* loop() { return loop_; }
  // 向客户端发送文本帧（线程安全）
  void reply(const std::string& text);
  const std::shared_ptr<Connection>& conn() const { return conn_; }

  // 登录态：AppServer 校验成功后调用 setLogin 记录用户信息
  int64_t userId() const { return userId_.load(); }
  bool isLoggedIn() const { return userId_.load() >= 0; }
  const std::string& token() const { return token_; }
  void setLogin(int64_t userId, const std::string& token) {
    userId_.store(userId);
    token_ = token;
  }
  void logout() {
    userId_.store(-1);
    token_.clear();
  }

private:
  void onMessage(const std::shared_ptr<Connection>&,
               const char* data, size_t len);
  void onClose(const std::shared_ptr<Connection>&);
  void processFrames();
  void handle(const ws::Frame& frame);
  void handleData(ws::Opcode opcode, const std::string& payload);
  void handleHandshake(const std::string& request);
  // 握手失败：回 403 响应，先冲刷响应字节再关闭连接（在连接所属 loop 线程执行）
  void rejectHandshake(const std::string& reason);

  // 保证同一会话的消息按序、串行被线程池处理
  std::mutex queueMutex_;
  std::deque<std::string> pendingData_;
  size_t queuedBytes_ = 0; // pendingData_ 中累计字节数（队列锁保护）
  bool draining_ = false;
  bool queueOverflow_ = false; // 读方向队列超限，后续数据一律丢弃并断开
  void enqueueAndDrain(const std::shared_ptr<Connection>&, std::string data);
  void drain();

  EventLoop* loop_;
  ThreadPool* pool_;
  std::shared_ptr<Connection> conn_;
  int fd_; // 连接 fd，预先缓存供日志使用（连接关闭后 fd 会被置 -1）
  std::vector<std::string> allowedOrigins_; // 允许的 Origin/Host；空表示不校验
  std::function<void()> closedHandler_;
  TextHandler textHandler_; // 业务回调（不持有，由上层注入）

  // 生命周期守卫：start() 中自持，onClose 中释放。
  // Connection 的回调只捕获 weak_ptr<Session>，从而打破
  // Session(持有 shared_ptr<Connection>) <-> Connection(持有 shared_ptr<Session>)
  // 的循环引用，避免会话与连接永久泄漏。
  std::shared_ptr<Session> guard_;

  bool handshaked_ = false;
  std::string httpBuffer_;
  ws::FrameParser parser_;
  // 登录态：-1 表示未登录；登录成功后记录 userId 与 token
  // userId_ 可被工作线程（业务回调）写、loop 线程（onClose 回调）读，需原子
  std::atomic<int64_t> userId_{-1};
  std::string token_;
  // 分片重组缓冲
  bool fragmenting_ = false;
  ws::Opcode fragmentOpcode_ = ws::Opcode::Text;
  std::string fragmentBuffer_;

  // 心跳：服务器定时发 Ping，客户端须回 Pong；连续超时判为僵尸连接并断开
  EventLoop::TimerId heartbeatTimer_ = 0;
  // 追踪最后一次收到任意消息的时间（loop 线程访问）
  std::chrono::steady_clock::time_point lastRecv_{};
  std::chrono::steady_clock::time_point lastPingSent_{};
};

} // namespace im