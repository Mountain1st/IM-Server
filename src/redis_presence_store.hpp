#pragma once

#include "presence_store.hpp"
#include "redis_client.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace im {

// Presence 的 Redis 后端：p:online:<userId> 存连接计数（INCR/DECR）。
// 每次计数变动刷新 TTL（1 小时）：正常登出主动归零删除；
// 进程崩溃来不及清理时由 TTL 兜底消除僵尸键。
// 键带 TTL 而非长期存在的原因：单实例崩溃后无法主动撤销，
// 在线状态允许最多 1 小时的误差换取最终一致。
class RedisPresenceStore : public PresenceStore {
 public:
  RedisPresenceStore(const std::string& host, unsigned port,
                     const std::string& password, int db);

  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  void online(int64_t userId) override;
  void offline(int64_t userId) override;
  bool isOnline(int64_t userId) override;

 private:
  RedisClient client_;
};

} // namespace im
