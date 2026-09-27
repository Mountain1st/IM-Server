#pragma once

#include "presence_store.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace im {

class Session;

// 在线会话路由表：userId -> 该用户所有在线会话（支持一账号 Qt/Web 多端同时在线）。
// 工作线程池与 IO 线程都会访问，内部用互斥锁保护。
// 同时聚合 PresenceStore：add/remove 会话时同步维护在线计数（内存/Redis 双后端），
// 业务侧经 presence() 查询在线状态下发到 contact.list / group.members。
class OnlineManager {
public:
  // presence 缺省为空（仅本地会话表，不维护在线标记）
  explicit OnlineManager(std::shared_ptr<PresenceStore> presence = nullptr);
  // 用户上线一个会话（同一账号可在多端同时上线）。重复添加无副作用。
  void add(int64_t userId, const std::weak_ptr<Session>& ws);
  // 会话下线：移除指定会话，若该用户已无任何存活会话则删除整项。
  void remove(int64_t userId, const std::shared_ptr<Session>& session);
  // 返回用户当前在线会话快照（仅存活会话，供消息广播）。
  std::vector<std::shared_ptr<Session>> sessions(int64_t userId);
  // 该用户当前是否有在线会话。
  bool isOnline(int64_t userId);
  // 在线状态查询（PresenceStore 后端；未注入时退化为本地会话表判断）
  bool isOnlinePresence(int64_t userId);

private:
  std::mutex mutex_;
  // owner_less 使 weak_ptr 可比较/哈希；借用其自动清理过期引用的语义
  std::unordered_map<int64_t,
      std::set<std::weak_ptr<Session>, std::owner_less<std::weak_ptr<Session>>>> map_;
  std::shared_ptr<PresenceStore> presence_; // 在线标记（可空）
};

} // namespace im
