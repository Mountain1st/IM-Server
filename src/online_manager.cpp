#include "online_manager.hpp"

#include "session.hpp"

namespace im {

OnlineManager::OnlineManager(std::shared_ptr<PresenceStore> presence)
    : presence_(std::move(presence)) {}

void OnlineManager::add(int64_t userId, const std::weak_ptr<Session>& ws) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[userId].insert(ws);
  }
  // Redis 调用放锁外：presence 后端可能阻塞（远程 Redis/停顿），
  // 锁内等待会把消息扇出等共享本锁的路径一并拖死（2026-09-27 审计）。
  // 锁外与 remove 的交错仅影响 Presence 计数的瞬时一致性，由 TTL/下次事件自愈。
  if (presence_) presence_->online(userId);
}

void OnlineManager::remove(int64_t userId, const std::shared_ptr<Session>& session) {
  bool erased = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(userId);
    if (it != map_.end()) {
      erased = it->second.erase(std::weak_ptr<Session>(session)) > 0;
      if (it->second.empty()) map_.erase(it); // 无存活会话则删除该用户项
    }
  }
  // 与 add 一一对应：每成功移除一个会话减一（多端场景计数保持对称）
  if (erased && presence_) presence_->offline(userId);
}

bool OnlineManager::isOnlinePresence(int64_t userId) {
  // 注入了 PresenceStore 走外部标记（Redis 模式），否则退化为本地会话表
  if (presence_) return presence_->isOnline(userId);
  return isOnline(userId);
}

std::vector<bool> OnlineManager::isOnlinePresenceBatch(
    const std::vector<int64_t>& userIds) {
  if (presence_) return presence_->isOnlineBatch(userIds);
  // 内存退化：单锁收集条目存在性（列表展示容忍弱引用瞬时误差）
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<bool> out;
  out.reserve(userIds.size());
  for (int64_t uid : userIds) {
    auto it = map_.find(uid);
    out.push_back(it != map_.end() && !it->second.empty());
  }
  return out;
}

std::vector<std::shared_ptr<Session>> OnlineManager::sessions(int64_t userId) {
  std::vector<std::shared_ptr<Session>> out;
  int expired = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(userId);
    if (it == map_.end()) return out;
    // 顺手清除过期条目，防止 onLogin 的 close 竞态等场景残留弱引用导致表膨胀
    for (auto w = it->second.begin(); w != it->second.end();) {
      if (auto s = w->lock()) {
        out.push_back(std::move(s));
        ++w;
      } else {
        w = it->second.erase(w);
        ++expired;
      }
    }
    if (it->second.empty()) map_.erase(it);
  }
  // 锁外补偿：每个被清除的过期会话对称减一（对应 add 时的 online）
  if (expired > 0 && presence_) {
    for (int i = 0; i < expired; ++i) presence_->offline(userId);
  }
  return out;
}

bool OnlineManager::isOnline(int64_t userId) {
  int expired = 0;
  bool online = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(userId);
    if (it != map_.end()) {
      for (auto w = it->second.begin(); w != it->second.end();) {
        if (w->expired()) {
          w = it->second.erase(w);
          ++expired;
          continue;
        }
        online = true;
        break;
      }
      if (it->second.empty()) map_.erase(it);
    }
  }
  if (expired > 0 && presence_) {
    for (int i = 0; i < expired; ++i) presence_->offline(userId);
  }
  return online;
}

} // namespace im
