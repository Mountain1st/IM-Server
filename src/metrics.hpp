#pragma once

#include <atomic>
#include <cstdint>

namespace im {

// 全局原子指标（P1-4）：轻量计数器，供 /health 端点与观测使用。
// 全部为内存原子计数，无锁、可并发自增。
struct Metrics {
  std::atomic<uint64_t> connectionsAccepted{0}; // 累计接受连接
  std::atomic<uint64_t> connectionsClosed{0};   // 累计关闭连接
  std::atomic<uint64_t> activeConnections{0};   // 当前活动连接
  std::atomic<uint64_t> handshakeFailures{0};   // WebSocket 握手失败/拒绝
  std::atomic<uint64_t> protocolErrors{0};      // 协议错误断开
  std::atomic<uint64_t> authFailures{0};        // 登录失败（403/429）
  std::atomic<uint64_t> rateLimited{0};         // msg.send 限流拒绝
  std::atomic<uint64_t> messagesPushed{0};      // 消息推送成功数（msg.recv 写出，≠已确认）
  std::atomic<uint64_t> messagesDelivered{0};   // 客户端确认投递数（ACK 驱动）
  std::atomic<uint64_t> messagesOffline{0};     // 离线入队消息数
  std::atomic<uint64_t> writeOverflows{0};      // 写缓冲超限断开
  // 媒体（任务② M1：commit 内容校验；M2：孤儿回收）
  std::atomic<uint64_t> mediaCommitRejected{0};      // commit 内容校验拒绝数（magic/MIME/维度）
  std::atomic<uint64_t> mediaGcPendingReclaimed{0};  // GC 成功回收的超期 PENDING 数
  std::atomic<uint64_t> mediaGcOrphanReclaimed{0};   // GC 成功回收的无引用 COMMITTED 数
  std::atomic<uint64_t> mediaGcDeleteFailed{0};      // GC 删对象失败数（状态已回滚待重试）
};

extern Metrics g_metrics;

} // namespace im