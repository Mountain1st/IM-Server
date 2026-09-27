#include "online_manager.hpp"

#include "session.hpp"

namespace im {

OnlineManager::OnlineManager(std::shared_ptr<PresenceStore> presence)
    : presence_(std::move(presence)) {}

void OnlineManager::add(int64_t userId, const std::weak_ptr<Session>& ws) {
  std::lock_guard<std::mutex> lock(mutex_);
  map_[userId].insert(ws);
  // 同步维护在线标记（锁外调用会与 remove 的时序交错，锁内串行更稳）
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

std::vector<std::shared_ptr<Session>> OnlineManager::sessions(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<Session>> out;
  auto it = map_.find(userId);
  if (it == map_.end()) return out;
  // 顺手清除过期条目，防止 onLogin 的 close 竞态等场景残留弱引用导致表膨胀
  int expired = 0;
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
  if (expired > 0 && presence_) {
    // 过期弱引用兜底清理：每个被清除的会话对称减一（对应 add 时的 online）
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
