#include "offline_store.hpp"

namespace im {

bool OfflineStore::push(int64_t userId, const json& msg) {
  std::lock_guard<std::mutex> lock(mutex_);
  cleanExpiredLocked();
  auto now = std::chrono::steady_clock::now();
  auto& q = store_[userId];
  // TTL 过期或超上限：丢弃最旧，保持队列有界
  while (!q.empty() && now - q.front().storedAt >= config::kOfflineTtl) q.pop_front();
  while (q.size() >= config::kOfflineMaxPerUser) q.pop_front();
  q.push_back(Entry{msg, now});
  return true;
}

std::vector<json> OfflineStore::peek(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  cleanExpiredLocked();
  auto it = store_.find(userId);
  if (it == store_.end()) return {};
  std::vector<json> out;
  out.reserve(it->second.size());
  for (const auto& e : it->second) out.push_back(e.msg);
  return out;
}

void OfflineStore::cleanExpiredLocked() {
  auto now = std::chrono::steady_clock::now();
  for (auto it = store_.begin(); it != store_.end();) {
    auto& q = it->second;
    while (!q.empty() && now - q.front().storedAt >= config::kOfflineTtl) q.pop_front();
    if (q.empty()) it = store_.erase(it);
    else ++it;
  }
}

} // namespace im