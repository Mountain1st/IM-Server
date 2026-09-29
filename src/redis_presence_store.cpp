// ============================================================
// RedisPresenceStore：在线状态的 Redis 后端（观察面）
// - 键 p:online:<userId> = 连接计数，INCR/DECR；TTL 1 小时兜底
//   （断连漏减时靠 TTL 自愈归零）
// - isOnlineBatch：MGET 一次批量判定（列表接口在线字段的数据源）
// 失败按"全部离线"降级（Presence 仅观测，不影响投递）
// ============================================================
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
  // commandInt 失败返回 -2（连接错误/超时）：按"未知/离线"处理
  return client_.commandInt(
             {"EXISTS", "p:online:" + std::to_string(userId)}) == 1;
}

std::vector<bool> RedisPresenceStore::isOnlineBatch(
    const std::vector<int64_t>& userIds) {
  // 一次 MGET 替代 N 次 EXISTS：列表接口（contact.list/group.members）N+1 优化。
  // 失败（连接错误/超时）统一按离线降级，不影响主业务。
  const std::vector<bool> kAllOffline(userIds.size(), false);
  if (userIds.empty()) return {};
  std::vector<std::string> args{"MGET"};
  args.reserve(userIds.size() + 1);
  for (int64_t uid : userIds) args.push_back("p:online:" + std::to_string(uid));
  std::vector<std::string> vals;
  if (!client_.commandStringArray(args, &vals)) return kAllOffline;
  if (vals.size() != userIds.size()) return kAllOffline; // 长度不符按降级
  std::vector<bool> out;
  out.reserve(vals.size());
  for (const std::string& v : vals) {
    out.push_back(!v.empty() && v != "0");
  }
  return out;
}

} // namespace im
