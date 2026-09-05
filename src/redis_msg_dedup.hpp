#pragma once

#include "msg_dedup.hpp"
#include "token_store.hpp"
#include "redis_client.hpp"

namespace im {

// Redis 幂等去重：SET key 1 EX 窗口 NX。
// key = dedup:<userId>:<clientMsgId>；NX 保证窗口内只记录一次。
class RedisMsgDedup : public MsgDedupStore {
public:
  explicit RedisMsgDedup(const RedisStoreConfig& cfg);
  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  bool isDuplicate(int64_t userId, const std::string& clientMsgId) override;
  void remember(int64_t userId, const std::string& clientMsgId) override;
  bool checkAndRemember(int64_t userId, const std::string& clientMsgId) override;

private:
  std::string key(int64_t userId, const std::string& clientMsgId) const;
  static constexpr long kWindowSeconds = 300; // 5 分钟
  RedisClient client_;
};

} // namespace im