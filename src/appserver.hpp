#pragma once

#include "message.hpp"
#include "message_repository.hpp"
#include "msg_dedup.hpp"
#include "online_manager.hpp"
#include "rate_limiter.hpp"
#include "tcpserver.hpp"
#include "threadpool.hpp"
#include "user_store.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace im {

class Session;
class EventLoop;

// 业务层（外层封装）：
// - 拥有工作线程池，负责业务逻辑（注册/登录/消息路由/离线补发），不直接进行客户端 IO；
// - 内部组合网络层 TcpServer（拥有主从 eventloop）作为成员；
// 启动/退出都经由本层：通知各工作线程，再通知 TcpServer，
// 由 TcpServer 通知主从 eventloop。
class AppServer : public std::enable_shared_from_this<AppServer> {
public:
  AppServer(uint16_t port, size_t ioThreads, size_t workThreads);
  AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections);
  // allowedOrigins：允许的 Origin/Host 列表（条目格式见 websocket.hpp），
  // 为空表示握手不校验 Origin/Host（向后兼容）。
  AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
            std::vector<std::string> allowedOrigins);
  // userConfig：MySQL 连接配置（必需；缺失/连接失败则启动报错退出）
  AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
            std::vector<std::string> allowedOrigins, UserStoreConfig userConfig);
  // redisConfig：Redis 连接配置（IM_REDIS_ENABLED=1 启用 token 等存储到 Redis；
  // 连接失败则启动报错退出）
  AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
            std::vector<std::string> allowedOrigins, UserStoreConfig userConfig,
            RedisStoreConfig redisConfig);
  ~AppServer();

  void start();
  // 最小停止请求（async-signal-safe，仅供信号处理器调用）：
  // 只唤醒主事件循环使其退出；完整优雅关闭 stop() 由主线程随后执行
  void requestStop();
  void stop();
  void run(); // 运行主 eventloop（阻塞），供 main 调用

private:
  void onNewConnection(EventLoop* ioLoop, const TcpServer::ConnectionPtr& conn);
  void onSessionClosed(const std::shared_ptr<Session>& session);
  // 业务指令路由：在工作线程池线程执行
  void onEnvelope(const std::shared_ptr<Session>& session, const json& env);
  void onRegister(const std::shared_ptr<Session>& session, const json& data);
  void onLogin(const std::shared_ptr<Session>& session, const json& data);
  void onToken(const std::shared_ptr<Session>& session, const json& data);
  void onLogout(const std::shared_ptr<Session>& session);
  void onSend(const std::shared_ptr<Session>& session, const json& data);
  // 新建对话：校验目标 userId 是否存在并返回其昵称
  void onCreateConv(const std::shared_ptr<Session>& session, const json& data);
  // 离线消息确认：客户端已收到 msg.offline，标记为已投递
  void onOfflineAck(const std::shared_ptr<Session>& session, const json& data);
  // 消息历史：拉取与目标用户的单聊历史
  void onHistory(const std::shared_ptr<Session>& session, const json& data);
  // 已读回执：记录与某用户的消息已读
  void onRead(const std::shared_ptr<Session>& session, const json& data);
  // 生成全局消息 ID（时间戳前缀 + 进程内自增序列，防重启碰撞）
  std::string nextMessageId();
  // 登录/重连后补发离线消息（从消息存储读取，非破坏性）
  void pushOfflineMessages(int64_t userId, const std::shared_ptr<Session>& session);

  std::shared_ptr<TcpServer> tcpServer_;
  ThreadPool workPool_;
  std::vector<std::string> allowedOrigins_; // 握手 Origin/Host 允许列表；空=不校验
  std::unique_ptr<UserStore> userStore_;    // MySQL 用户存储（连接失败时构造抛错）
  std::unique_ptr<MessageRepository> msgRepo_; // MySQL 消息存储（离线/历史/已读）
  OnlineManager onlineMgr_;
  MsgDedup msgDedup_;
  RateLimiter msgLimiter_; // msg.send 每用户限流
  std::atomic<uint64_t> msgSeq_{0}; // 全局消息序列（生成 messageId）
};

} // namespace im
