#include "redis_msg_dedup.hpp"

namespace im {

RedisMsgDedup::RedisMsgDedup(const RedisStoreConfig& cfg)
  : client_(cfg.redisHost, cfg.redisPort, cfg.redisPassword, cfg.redisDb) {}

std::string RedisMsgDedup::key(int64_t userId, const std::string& clientMsgId) const {
  return "dedup:" + std::to_string(userId) + ":" + clientMsgId;
}

bool RedisMsgDedup::isDuplicate(int64_t userId, const std::string& clientMsgId) {
  if (clientMsgId.empty()) return false;
  // EXISTS 返回 1=已存在（重复）0=不存在
  return client_.commandInt({"EXISTS", key(userId, clientMsgId)}) == 1;
}

void RedisMsgDedup::remember(int64_t userId, const std::string& clientMsgId) {
  if (clientMsgId.empty()) return;
  // SET key 1 EX 300 NX：NX 保证窗口内只写一次
  client_.commandStatus({"SET", key(userId, clientMsgId), "1",
                         "EX", std::to_string(kWindowSeconds), "NX"});
}

bool RedisMsgDedup::checkAndRemember(int64_t userId, const std::string& clientMsgId) {
  if (clientMsgId.empty()) return false;
  // SET NX 返回 1=首次写入（非重复）0=已存在（重复）
  const int64_t rc = client_.commandStatus({"SET", key(userId, clientMsgId), "1",
                                            "EX", std::to_string(kWindowSeconds), "NX"});
  return rc != 1; // 未能写入=窗口内已存在
}

} // namespace im