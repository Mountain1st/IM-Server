#include "redis_presence_store.hpp"

namespace im {

namespace {
constexpr int kPresenceTtlSec = 3600; // 在线键 TTL：兜底进程崩溃的僵尸键
} // namespace

RedisPresenceStore::RedisPresenceStore(const std::string& host, unsigned port,
                                       const std::string& password, int db)
    : client_(host, port, password, db) {}

void RedisPresenceStore::online(int64_t userId) {
  // INCR 计数并刷新 TTL；命令失败时静默降级（在线查询将短暂失准）
  const std::string key = "p:online:" + std::to_string(userId);
  client_.commandInt({"INCR", key});
  client_.commandInt({"EXPIRE", key, std::to_string(kPresenceTtlSec)});
}

void RedisPresenceStore::offline(int64_t userId) {
  const std::string key = "p:online:" + std::to_string(userId);
  const int64_t n = client_.commandInt({"DECR", key});
  // 归零（或防御性负数）即删除，避免 EXISTS 误判在线
  if (n <= 0) {
    client_.commandInt({"DEL", key});
  }
}

bool RedisPresenceStore::isOnline(int64_t userId) {
  // commandInt 失败返回 -2（连接错误）：按"未知/离线"处理
  return client_.commandInt(
             {"EXISTS", "p:online:" + std::to_string(userId)}) == 1;
}

} // namespace im
