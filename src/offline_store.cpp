#include "offline_store.hpp"

namespace im {

void OfflineStore::push(int64_t userId, const json& msg) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& q = store_[userId];
  if (q.size() >= kMaxPerUser) q.pop_front(); // 超限丢最旧
  q.push_back(msg);
}

std::vector<json> OfflineStore::take(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = store_.find(userId);
  if (it == store_.end()) return {};
  std::vector<json> out(it->second.begin(), it->second.end());
  store_.erase(it); // 取走后清空
  return out;
}

} // namespace im
