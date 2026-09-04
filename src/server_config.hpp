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
// 登录账号长度上限
inline constexpr size_t kMaxAccountLen = 64;
// 昵称长度上限（<32）
inline constexpr size_t kMaxUsernameLen = 32;
// 密码长度范围
inline constexpr size_t kMinPasswordLen = 6;
inline constexpr size_t kMaxPasswordLen = 32;
// msg.send 发送限流：窗口时长与额度（携带/空 clientMsgId）
inline constexpr std::chrono::seconds kSendWindow(10);
inline constexpr size_t kMaxSendPerWindow = 30;
inline constexpr size_t kMaxSendNoIdPerWindow = 5;

// ---- 存储（内存态，防膨胀） ----
// 每用户离线消息上限
inline constexpr size_t kOfflineMaxPerUser = 100;
// 离线消息保留时长（TTL）：到期自动清理，超限时丢最旧
inline constexpr std::chrono::hours kOfflineTtl = std::chrono::hours(7 * 24);
// 幂等去重窗口与每用户 id 上限
inline constexpr std::chrono::minutes kDedupWindow = std::chrono::minutes(5);
inline constexpr size_t kDedupMaxPerUser = 1024;
// token 有效期（7 天）
inline constexpr std::chrono::hours kTokenTtl = std::chrono::hours(7 * 24);
// 登录限流：窗口内失败次数上限与窗口时长
inline constexpr int kMaxLoginFailures = 5;
inline constexpr std::chrono::seconds kLockWindow(60);

} // namespace config
} // namespace im