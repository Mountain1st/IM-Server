#pragma once

#include "message_repository.hpp"
#include "mysql_client.hpp"

#include <mutex>

namespace im {

// 消息存储的 MySQL 实现（messages 表）。
// MySQL 单连接非线程安全：内部用互斥锁串行化所有 DB 访问。
class MySqlMessageRepository : public MessageRepository {
public:
  MySqlMessageRepository(const std::string& host, unsigned port,
                         const std::string& user, const std::string& password,
                         const std::string& db);

  bool ok() const override { return client_.ok(); }
  bool saveMessage(const std::string& messageId, int64_t fromUser,
                   int64_t toUser, const std::string& content, int64_t ts,
                   const std::string& clientMsgId, const std::string& chatType,
                   int status, int64_t groupId = 0,
                   const std::string& msgType = "text",
                   int64_t mediaId = 0) override;
  std::vector<StoredMessage> fetchOffline(int64_t toUser) override;
  // 删除超过 TTL 仍未领取的单聊离线消息。返回删除条数。
  int64_t purgeExpiredOffline(int64_t nowSec) override;
  // 保留期清理：删除超期已消费消息与群消息。返回删除条数。
  int64_t purgeAgedHistory(int64_t nowSec, int retentionDays) override;
  void markDelivered(int64_t toUser,
                     const std::vector<std::string>& messageIds) override;
  std::vector<StoredMessage> fetchHistory(int64_t userA, int64_t userB,
                                          int limit) override;
  void markRead(int64_t toUser, int64_t fromUser, int64_t readTs) override;
  RecallResult recallMessage(const std::string& messageId, int64_t senderId,
                             int64_t nowSec, int64_t windowSec,
                             RecallInfo* info) override;
  // 两用户间是否存在单聊消息记录（任一方向）
  bool hasConversation(int64_t userA, int64_t userB) override;
  // 媒体访问可达性：单聊 from/to=userId，或 userId 为引用该媒体的群消息群成员。
  // V1 保守放行（存在引用即允许，不校验群消息可见窗口）；查询失败 fail-closed。
  bool hasMediaAccess(int64_t userId, const std::string& fileId) override;
  std::vector<StoredMessage> fetchGroupMessages(int64_t groupId,
                                                int64_t sinceTs,
                                                int64_t untilTs,
                                                int limit, bool asc) override;
  // 会话列表（v3.2）：单聊派生聚合 + 末条点查 + 群末条 ts 聚合
  std::vector<ConversationEntry> fetchSingleConversations(int64_t userId) override;
  bool fetchLastMessageSingle(int64_t userA, int64_t userB,
                              LastMessageInfo* out) override;
  bool fetchLastMessageGroup(int64_t groupId, LastMessageInfo* out) override;
  std::vector<std::pair<int64_t, int64_t>> fetchGroupLastTs(
      const std::vector<int64_t>& groupIds) override;

private:
  MySqlClient client_;
  std::mutex mutex_; // 串行化 DB 访问（单连接非线程安全）
};

} // namespace im