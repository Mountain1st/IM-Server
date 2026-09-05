#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace im {

// 一条已存储的单聊消息（含状态字段）。
struct StoredMessage {
  std::string messageId;  // 全局唯一 ID（对外）
  int64_t fromUser;
  int64_t toUser;
  std::string content;
  int64_t ts;             // unix 秒
  std::string clientMsgId;
  std::string chatType;
  int status;             // 0=offline_pending 1=delivered 2=read
};

// 消息状态（与 messages 表 status 列一致）
enum MessageStatus {
  kMsgOfflinePending = 0,
  kMsgDelivered = 1,
  kMsgRead = 2,
};

// 消息存储接口：消息的持久化（在线/离线/历史）统一委托给本接口。
// 当前唯一实现为 MySQL（MySqlMessageRepository）。实现须自身保证线程安全
// （MySQL 单连接非线程安全，由实现内部加互斥锁串行化）。
class MessageRepository {
public:
  virtual ~MessageRepository() = default;

  // 存储是否可用（连接是否成功）。
  virtual bool ok() const = 0;

  // 保存一条消息。成功返回 true 并回填 messageId（如实现自生成则覆盖传入值）。
  virtual bool saveMessage(const std::string& messageId, int64_t fromUser,
                           int64_t toUser, const std::string& content,
                           int64_t ts, const std::string& clientMsgId,
                           const std::string& chatType, int status) = 0;
  // 取某接收方的离线消息（status=offline_pending），按 ts 升序。不移除。
  virtual std::vector<StoredMessage> fetchOffline(int64_t toUser) = 0;
  // 批量标记离线消息为 delivered（客户端 msg.offline_ack 确认）。
  virtual void markDelivered(int64_t toUser,
                             const std::vector<std::string>& messageIds) = 0;
  // 拉取 userA 与 userB 之间的单聊历史，按 ts 倒序（最新的在前），limit 条。
  virtual std::vector<StoredMessage> fetchHistory(int64_t userA, int64_t userB,
                                                  int limit) = 0;
  // 把接收方 toUser 与 fromUser 之间、readTs 之前未读的消息标记为已读。
  virtual void markRead(int64_t toUser, int64_t fromUser, int64_t readTs) = 0;
};

} // namespace im