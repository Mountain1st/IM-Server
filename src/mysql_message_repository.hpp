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
                   int status) override;
  std::vector<StoredMessage> fetchOffline(int64_t toUser) override;
  void markDelivered(int64_t toUser,
                     const std::vector<std::string>& messageIds) override;
  std::vector<StoredMessage> fetchHistory(int64_t userA, int64_t userB,
                                          int limit) override;
  void markRead(int64_t toUser, int64_t fromUser, int64_t readTs) override;

private:
  MySqlClient client_;
  std::mutex mutex_; // 串行化 DB 访问（单连接非线程安全）
};

} // namespace im