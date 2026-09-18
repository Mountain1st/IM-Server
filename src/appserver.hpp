#pragma once

#include "contact_repository.hpp"
#include "group_repository.hpp"
#include "message.hpp"
#include "message_repository.hpp"
#include "msg_dedup.hpp"
#include "online_manager.hpp"
#include "rate_limiter_store.hpp"
#include "server_config.hpp"
#include "tcpserver.hpp"
#include "threadpool.hpp"
#include "user_store.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
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
  // maxPerIpConnections：单 IP 并发连接上限（0 表示不限；缺省见 server_config）
  AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
            std::vector<std::string> allowedOrigins, UserStoreConfig userConfig,
            RedisStoreConfig redisConfig,
            size_t maxPerIpConnections = config::kMaxPerIpConnections);
  ~AppServer();

  void start();
  // 最小停止请求（async-signal-safe，仅供信号处理器调用）：
  // 只唤醒主事件循环使其退出；完整优雅关闭 stop() 由主线程随后执行
  void requestStop();
  void stop();
  void run(); // 运行主 eventloop（阻塞），供 main 调用

private:
  // 离线消息 TTL 清理循环（独立线程）：周期删除超期未领取的单聊离线消息
  void purgeLoop();

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
  void onSearch(const std::shared_ptr<Session>& session, const json& data);
  void onContactRequest(const std::shared_ptr<Session>& session, const json& data);
  void onContactAccept(const std::shared_ptr<Session>& session, const json& data);
  void onContactReject(const std::shared_ptr<Session>& session, const json& data);
  void onContactList(const std::shared_ptr<Session>& session, const json& data);
  void onContactRequests(const std::shared_ptr<Session>& session, const json& data);
  void onContactDelete(const std::shared_ptr<Session>& session, const json& data);
  void onSettingsGet(const std::shared_ptr<Session>& session, const json& data);
  void onSettingsSet(const std::shared_ptr<Session>& session, const json& data);
  void onGroupCreate(const std::shared_ptr<Session>& session, const json& data);
  void onGroupInvite(const std::shared_ptr<Session>& session, const json& data);
  void onGroupLeave(const std::shared_ptr<Session>& session, const json& data);
  void onGroupKick(const std::shared_ptr<Session>& session, const json& data);
  void onGroupTransfer(const std::shared_ptr<Session>& session, const json& data);
  void onGroupDissolve(const std::shared_ptr<Session>& session, const json& data);
  void onGroupRename(const std::shared_ptr<Session>& session, const json& data);
  void onGroupAnnounce(const std::shared_ptr<Session>& session, const json& data);
  void onGroupList(const std::shared_ptr<Session>& session, const json& data);
  void onGroupMembers(const std::shared_ptr<Session>& session, const json& data);
  void onGroupHistory(const std::shared_ptr<Session>& session, const json& data);
  void onGroupSend(const std::shared_ptr<Session>& session, const json& data,
                   const std::string& content, const std::string& clientMsgId);
  // 推送 pending 好友请求（登录/重连后补发）
  void pushPendingRequests(int64_t userId, const std::shared_ptr<Session>& session);
  // 生成全局消息 ID（时间戳前缀 + 进程内自增序列，防重启碰撞）
  std::string nextMessageId();
  // 登录/重连后补发离线消息（从消息存储读取，非破坏性）
  void pushOfflineMessages(int64_t userId, const std::shared_ptr<Session>& session);

  std::shared_ptr<TcpServer> tcpServer_;
  ThreadPool workPool_;
  std::vector<std::string> allowedOrigins_; // 握手 Origin/Host 允许列表；空=不校验
  std::unique_ptr<UserStore> userStore_;    // MySQL 用户存储（连接失败时构造抛错）
  std::unique_ptr<MessageRepository> msgRepo_; // MySQL 消息存储（离线/历史/已读）
  std::unique_ptr<ContactRepository> contactRepo_; // 联系人/请求/设置存储
  std::unique_ptr<GroupRepository> groupRepo_;  // 群/成员/水位线存储
  OnlineManager onlineMgr_;
  std::unique_ptr<MsgDedupStore> msgDedup_;    // 消息幂等去重（内存/Redis 双后端）
  std::unique_ptr<RateLimiterStore> msgLimiter_; // msg.send 限流（内存/Redis 双后端）
  std::atomic<uint64_t> msgSeq_{0}; // 全局消息序列（生成 messageId）
  // 离线 TTL 清理线程（间隔秒由 IM_OFFLINE_PURGE_INTERVAL 覆盖，缺省 1800）
  int purgeIntervalSec_ = config::kOfflinePurgeIntervalSec;
  std::thread purgeThread_;
  std::mutex purgeMutex_;
  std::condition_variable purgeCv_;
  std::atomic<bool> purgeStop_{false};
};

} // namespace im
