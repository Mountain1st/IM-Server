#pragma once

#include "message.hpp"
#include "msg_dedup.hpp"
#include "offline_store.hpp"
#include "online_manager.hpp"
#include "tcpserver.hpp"
#include "threadpool.hpp"
#include "user_store.hpp"

#include <cstdint>
#include <memory>

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

  std::shared_ptr<TcpServer> tcpServer_;
  ThreadPool workPool_;
  UserStore userStore_;
  OnlineManager onlineMgr_;
  OfflineStore offlineStore_;
  MsgDedup msgDedup_;
};

} // namespace im
