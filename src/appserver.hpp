#pragma once

#include "block_repository.hpp"
#include "contact_repository.hpp"
#include "group_repository.hpp"
#include "media_repository.hpp"
#include "media_service.hpp"
#include "message.hpp"
#include "message_repository.hpp"
#include "msg_dedup.hpp"
#include "object_store.hpp"
#include "online_manager.hpp"
#include "presence_store.hpp"
#include "rate_limiter_store.hpp"
#include "server_config.hpp"
#include "tcpserver.hpp"
#include "threadpool.hpp"
#include "user_store.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
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
  // trustedProxies：可信反向代理 IP（IM_TRUSTED_PROXIES，逗号分隔）——对端在列表
  // 内时握手日志显示 X-Forwarded-For 的真实客户端 IP；空则忽略该头（默认）
  AppServer(uint16_t port, size_t ioThreads, size_t workThreads, size_t maxConnections,
            std::vector<std::string> allowedOrigins, UserStoreConfig userConfig,
            RedisStoreConfig redisConfig,
            size_t maxPerIpConnections = config::kMaxPerIpConnections,
            std::vector<std::string> trustedProxies = {});
  ~AppServer();

  void start();
  // 最小停止请求（async-signal-safe，仅供信号处理器调用）：
  // 只唤醒主事件循环使其退出；完整优雅关闭 stop() 由主线程随后执行
  void requestStop();
  void stop();
  void run(); // 运行主 eventloop（阻塞），供 main 调用

private:
  // 业务指令处理器统一签名（onLogout 也适配为忽略 data 的同构签名）
  using Handler = void (AppServer::*)(const std::shared_ptr<Session>&, const json&);
  // 指令类型 -> 处理器 的路由表（构造时 buildRoutes 一次性注册）
  std::unordered_map<std::string, Handler> routes_;
  void buildRoutes();

  // 离线消息 TTL 清理循环（独立线程）：周期删除超期未领取的单聊离线消息
  void purgeLoop();
  // 低频缓存比对：库内行数与内存缓存条目数不一致时全量重建
  // （应对绕过服务端直改数据库导致的缓存漂移；失败保持旧缓存）
  void syncCaches();

  void onNewConnection(EventLoop* ioLoop, const TcpServer::ConnectionPtr& conn);
  void onSessionClosed(const std::shared_ptr<Session>& session);
  // 业务指令路由：在工作线程池线程执行
  void onEnvelope(const std::shared_ptr<Session>& session, const json& env);
  void onRegister(const std::shared_ptr<Session>& session, const json& data);
  void onLogin(const std::shared_ptr<Session>& session, const json& data);
  void onToken(const std::shared_ptr<Session>& session, const json& data);
  void onLogout(const std::shared_ptr<Session>& session, const json& data);
  void onSend(const std::shared_ptr<Session>& session, const json& data);
  // 新建对话：校验目标 userId 是否存在并返回其昵称
  void onCreateConv(const std::shared_ptr<Session>& session, const json& data);
  // 离线消息确认：客户端已收到 msg.offline，标记为已投递
  void onOfflineAck(const std::shared_ptr<Session>& session, const json& data);
  // 消息历史：拉取与目标用户的单聊历史
  void onHistory(const std::shared_ptr<Session>& session, const json& data);
  // 已读回执：记录与某用户的消息已读
  void onRead(const std::shared_ptr<Session>& session, const json& data);
  void onRecall(const std::shared_ptr<Session>& session, const json& data);
  void onConversationList(const std::shared_ptr<Session>& session, const json& data);
  // 消息出站序列化（单一出口）：status=RECALLED 时剥离 content/mediaId，
  // 输出 recalled:true 占位符；正常消息与既有字段逐一对齐（行为不变）。
  // groupName 非空时追加 groupName 字段（群历史/群补发用）。
  json messageToJson(const StoredMessage& m, const std::string& groupName = "");
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
  // v2.4 拉黑（block.add/remove/list；语义见 docs/protocol.md §4）
  void onBlockAdd(const std::shared_ptr<Session>& session, const json& data);
  void onBlockRemove(const std::shared_ptr<Session>& session, const json& data);
  void onBlockList(const std::shared_ptr<Session>& session, const json& data);
  void onGroupSend(const std::shared_ptr<Session>& session, const json& data,
                   const std::string& content, const std::string& clientMsgId,
                   const std::string& msgType, int64_t mediaId = 0);
  // 媒体域（media.presign/commit/url，V1 对象存储直传）
  void onMediaPresign(const std::shared_ptr<Session>& session, const json& data);
  void onMediaCommit(const std::shared_ptr<Session>& session, const json& data);
  void onMediaUrl(const std::shared_ptr<Session>& session, const json& data);
  // 图片发送目标校验：目标解析 + 发送限流 + 权限只读检查。
  // 返回 0 通过；非 0 为应回的 ack code（ackMsg 回填文案）。
  // 注意：base64 图片管线已移除，本函数保留供 mediaId 图片消息复用目标/权限校验。
  int precheckImageTarget(const std::shared_ptr<Session>& session, const json& data,
                          const std::string& chatType, int64_t* toOut,
                          std::string* ackMsg);
  // 推送 pending 好友请求（登录/重连后补发）
  void pushPendingRequests(int64_t userId, const std::shared_ptr<Session>& session);
  // 生成全局消息 ID（时间戳前缀 + 进程内自增序列，防重启碰撞）
  std::string nextMessageId();
  // 登录/重连后补发离线消息（从消息存储读取，非破坏性）
  void pushOfflineMessages(int64_t userId, const std::shared_ptr<Session>& session);

  std::shared_ptr<TcpServer> tcpServer_;
  ThreadPool workPool_;
  std::vector<std::string> allowedOrigins_; // 握手 Origin/Host 允许列表；空=不校验
  std::vector<std::string> trustedProxies_; // 可信代理 IP；空=忽略 X-Forwarded-For
  std::unique_ptr<UserStore> userStore_;    // MySQL 用户存储（连接失败时构造抛错）
  std::unique_ptr<MessageRepository> msgRepo_; // MySQL 消息存储（离线/历史/已读）
  // 媒体元数据存储（media 表）：msgType=image 时按 fileId 校验归属/COMMITTED
  // 并取 media.id 落库。
  std::shared_ptr<MediaRepository> mediaRepo_;
  // 对象存储（S3/MinIO 预签名 + HEAD/DELETE）。环境变量 IM_S3_* 缺失时
  // 不 enabled，媒体功能降级（presign/commit/url 回 503）但服务仍起。
  std::shared_ptr<ObjectStore> objectStore_;
  std::unique_ptr<ContactRepository> contactRepo_; // 联系人/请求/设置存储
  std::unique_ptr<GroupRepository> groupRepo_;  // 群/成员/水位线存储
  std::unique_ptr<BlockRepository> blockRepo_;  // 拉黑存储（v2.4，第四个纳入自愈的缓存）
  std::shared_ptr<PresenceStore> presence_;  // 在线状态标记（内存/Redis 双后端）
  OnlineManager onlineMgr_;
  std::unique_ptr<MsgDedupStore> msgDedup_;    // 消息幂等去重（内存/Redis 双后端）
  std::unique_ptr<RateLimiterStore> msgLimiter_; // msg.send 限流（内存/Redis 双后端）
  std::unique_ptr<RateLimiterStore> queryLimiter_; // 重查询限流（history/members；Redis 键前缀 qrate: 独立额度桶）
  std::unique_ptr<RateLimiterStore> registerLimiter_; // 注册限流（按 IP；Redis 键前缀 reg: 独立额度桶）
  // 媒体限流（media.presign 按用户 30 次/分钟；Redis 键前缀 mrate: 独立额度桶）。
  // 用 shared_ptr 是因为 MediaService 持有其共享所有权。
  std::shared_ptr<RateLimiterStore> mediaLimiter_;
  // 媒体业务编排（presign/commit/url/gc）。依赖 objectStore_/mediaRepo_/mediaLimiter_，
  // 构造尾部装配；canAccess 回调在 url 调用时注入。
  std::unique_ptr<MediaService> mediaService_;
  // 注册限流额度（环境变量 IM_REGISTER_LIMIT_PER_WINDOW 可覆盖，测试豁免）
  size_t registerLimit_ = config::kRegisterLimitPerWindow;
  // 全局消息序列（生成 messageId）。起点随机化（构造播种）：id 格式为
  // <秒级时间戳>-<seq>，若固定从 1 起，同秒内重启前后会生成相同 id
  //（撞唯一键丢消息）；随机起点把该窗口概率降到约百万分之一
  std::atomic<uint64_t> msgSeq_{1000000 + std::random_device{}() % 1000000};
  // 离线 TTL 清理线程（间隔秒由 IM_OFFLINE_PURGE_INTERVAL 覆盖，缺省 1800）
  int purgeIntervalSec_ = config::kOfflinePurgeIntervalSec;
  int msgRetentionDays_ = config::kMsgRetentionDays; // 已消费消息保留天数；0=不清理
  // 媒体 GC 上次执行时间（unix 秒，0=未执行过）与最短间隔（秒，
  // 由 IM_MEDIA_GC_INTERVAL_SEC 覆盖，缺省 24 小时；测试用小值验证）
  int64_t lastMediaGc_ = 0;
  int mediaGcIntervalSec_ = config::kMediaGcIntervalSec;
  std::thread purgeThread_;
  std::mutex purgeMutex_;
  std::condition_variable purgeCv_;
  std::atomic<bool> purgeStop_{false};
};

} // namespace im
