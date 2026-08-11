#include "online_manager.hpp"

#include "session.hpp"

namespace im {

void OnlineManager::add(int64_t userId, const std::weak_ptr<Session>& ws) {
  std::lock_guard<std::mutex> lock(mutex_);
  map_[userId].insert(ws);
}

void OnlineManager::remove(int64_t userId, const std::shared_ptr<Session>& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = map_.find(userId);
  if (it == map_.end()) return;
  it->second.erase(std::weak_ptr<Session>(session));
  if (it->second.empty()) map_.erase(it); // 无存活会话则删除该用户项
}

std::vector<std::shared_ptr<Session>> OnlineManager::sessions(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<Session>> out;
  auto it = map_.find(userId);
  if (it == map_.end()) return out;
  // 顺手清除过期条目，防止 onLogin 的 close 竞态等场景残留弱引用导致表膨胀
  for (auto w = it->second.begin(); w != it->second.end();) {
    if (auto s = w->lock()) {
      out.push_back(std::move(s));
      ++w;
    } else {
      w = it->second.erase(w);
    }
  }
  if (it->second.empty()) map_.erase(it);
  return out;
}

bool OnlineManager::isOnline(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = map_.find(userId);
  if (it == map_.end()) return false;
  for (auto w = it->second.begin(); w != it->second.end();) {
    if (w->expired()) {
      w = it->second.erase(w);
      continue;
    }
    return true;
  }
  if (it->second.empty()) map_.erase(it);
  return false;
}

} // namespace im
