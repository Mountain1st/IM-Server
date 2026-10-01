#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

// 集中资源预算与安全限制（P0-4）：
// 所有连接/会话/业务/存储的输入限制与队列上限统一在此定义，
// 各源文件通过 im::config:: 引用，避免匿名命名空间中分散常量。
namespace im {
namespace config {

// ---- 连接与会话 ----
// 心跳间隔：每 30 秒发一个 Ping
inline constexpr std::chrono::seconds kHeartbeatInterval(30);
// 僵尸判定：超过 2 个心跳周期未收到任何数据/Pong，判为死连接
inline constexpr std::chrono::seconds kZombieTimeout(60);
// 握手截止：连接建立后须在此时限内完成 WebSocket 握手（防 Slowloris 慢速占槽）
inline constexpr std::chrono::seconds kHandshakeTimeout(10);
// 握手请求头大小上限（超出视为恶意，断开）
inline constexpr size_t kMaxHandshakeHeaderBytes = 8 * 1024;
// 读方向排队上限（Session 队列）：超过视为对端洪泛，断开
inline constexpr size_t kMaxQueuedBytes = 8 * 1024 * 1024;
// 单条消息（含分片累积）上限：防御恶意超大帧
inline constexpr size_t kMaxMessageBytes = 2 * 1024 * 1024;
// 单个 WebSocket 帧上限（解析层）
inline constexpr size_t kMaxFrameBytes = 64u * 1024 * 1024;
// 单 IP 并发连接上限（防单 IP 建数千连接耗尽 fd/内存；0 表示不限）
inline constexpr size_t kMaxPerIpConnections = 100;

// ---- Connection（网络读写） ----
// 读缓冲单次分配大小
inline constexpr size_t kReadBufferSize = 64 * 1024;
// 单次读事件累计上限：防 ET 洪泛导致瞬时大内存分配
inline constexpr size_t kMaxReadPerEvent = 1 * 1024 * 1024;
// 写缓冲上限：超过则视为慢消费/不可达连接，断开避免内存无限增长
inline constexpr size_t kMaxWriteBufferBytes = 8 * 1024 * 1024;
// 优雅关闭（写完剩余缓冲）的 deadline：超时强制关闭，防连接占用
inline constexpr std::chrono::seconds kCloseFlushDeadline(5);

// ---- 业务（指令参数） ----
// 消息内容长度上限（64KB）
inline constexpr size_t kMaxContentBytes = 64 * 1024;
// 图片消息（msgType=image）：base64 信封上限 1.5MB（解码后约 1.1MB，
// 先于 64KB 文本上限分流，图片不走文本路径）
inline constexpr size_t kMaxImageB64Bytes = 1536 * 1024;
// 图片发送限流：每用户每 60 秒最多 10 张（独立额度桶，见 AppServer::imgLimiter_）
inline constexpr std::chrono::seconds kImageWindow(60);
inline constexpr size_t kImagePerWindow = 10;
// 图片解码并发闸：同时最多 2 路解码落盘（防大图解码占满工作线程）
inline constexpr int kMaxConcurrentImageDecode = 2;
// 登录账号格式：10-11 位纯数字（如手机号）。
// register 与 chat.create 均按此校验，不合规回 400；
// auth.login 不校验格式（错误统一按 403 处理，兼容遗留短账号）。
inline constexpr size_t kMinAccountLen = 10;
inline constexpr size_t kMaxAccountLen = 11;
// 登录输入的宽松长度上限（仅防滥用，不做格式校验）
inline constexpr size_t kLoginAccountMaxLen = 64;
// 昵称长度上限（<32）
inline constexpr size_t kMaxUsernameLen = 32;
// 密码长度范围
inline constexpr size_t kMinPasswordLen = 6;
inline constexpr size_t kMaxPasswordLen = 32;
// msg.send 发送限流：窗口时长与额度（携带/空 clientMsgId）
inline constexpr std::chrono::seconds kSendWindow(10);
inline constexpr size_t kMaxSendPerWindow = 30;
inline constexpr size_t kMaxSendNoIdPerWindow = 5;
// 重查询限流（chat.history / group.history / group.members）：滑动窗口内每用户次数
inline constexpr std::chrono::seconds kQueryWindow(10);
inline constexpr size_t kQueryLimitPerWindow = 20;
// 注册限流（auth.register 按 IP）：PBKDF2 约 50-100ms/次且未登录即可调用，
// 无限流可被用作未认证 CPU DoS（打满工作线程拖垮消息投递）。
// 环境变量 IM_REGISTER_LIMIT_PER_WINDOW 可覆盖（测试豁免用大值）
inline constexpr std::chrono::seconds kRegisterWindow(600);
inline constexpr size_t kRegisterLimitPerWindow = 3;
// PBKDF2 全局并发上限：同一时刻最多 N 个登录/注册在计算慢哈希，
// 排队不占 CPU（纵深防御分布式多 IP 刷注册）
inline constexpr int kMaxConcurrentPbkdf2 = 2;
// clientMsgId 长度上限（64，与 messages.client_msg_id 列宽对齐，防 Redis 大 key）
inline constexpr size_t kMaxClientMsgIdBytes = 64;
// msg.offline_ack 单次确认数量上限（防单包触发海量 DB 更新）
inline constexpr size_t kMaxOfflineAckIds = 1000;

// ---- 存储（内存态，防膨胀） ----
// 每用户离线消息上限
inline constexpr size_t kOfflineMaxPerUser = 100;
// 离线消息保留时长（TTL）：到期自动清理，超限时丢最旧
inline constexpr std::chrono::hours kOfflineTtl = std::chrono::hours(7 * 24);
// 离线 TTL 清理的默认间隔（秒）；环境变量 IM_OFFLINE_PURGE_INTERVAL 可覆盖（测试用小值）
inline constexpr int kOfflinePurgeIntervalSec = 1800;
// 已送达/已读消息（含群消息）的保留天数：超期删除（历史归档语义）。
// 环境变量 IM_MSG_RETENTION_DAYS 可覆盖；0 = 不清理。
inline constexpr int kMsgRetentionDays = 90;
// 幂等去重窗口与每用户 id 上限
inline constexpr std::chrono::minutes kDedupWindow = std::chrono::minutes(5);
inline constexpr size_t kDedupMaxPerUser = 1024;
// token 有效期（7 天）
inline constexpr std::chrono::hours kTokenTtl = std::chrono::hours(7 * 24);
// 登录限流：窗口内失败次数上限与窗口时长
inline constexpr int kMaxLoginFailures = 5;
inline constexpr std::chrono::seconds kLockWindow(60);
// 工作线程池停机排水超时（毫秒）：超时后放弃队列剩余任务，
// 防止个别慢任务（DB 超时等）拖住停机流程
inline constexpr uint32_t kPoolDrainTimeoutMs = 5000;

} // namespace config
} // namespace im