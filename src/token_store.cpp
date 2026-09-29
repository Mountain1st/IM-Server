// ============================================================
// TokenStore（内存后端）：token 会话存储（7 天 TTL）
// token → (userId, username)。Redis 后端跨重启保留（见
// redis_token_store.cpp）
// ============================================================
#include "token_store.hpp"

#include <iostream>

namespace im {

void MemoryTokenStore::put(const std::string& tokenDigest, int64_t userId,
                           std::chrono::seconds ttl) {
  std::lock_guard<std::mutex> lock(mutex_);
  tokens_[tokenDigest] = Entry{userId,
                               std::chrono::steady_clock::now() + ttl};
  if (tokens_.size() > kCleanupThreshold) cleanLocked();
}

int MemoryTokenStore::get(const std::string& tokenDigest, int64_t* userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  auto it = tokens_.find(tokenDigest);
  if (it == tokens_.end() || it->second.expireAt <= now) {
    if (it != tokens_.end()) tokens_.erase(it); // 顺手清掉过期项
    return 1;
  }
  *userId = it->second.userId;
  return 0;
}

void MemoryTokenStore::remove(const std::string& tokenDigest) {
  std::lock_guard<std::mutex> lock(mutex_);
  tokens_.erase(tokenDigest);
}

void MemoryTokenStore::cleanLocked() {
  auto now = std::chrono::steady_clock::now();
  for (auto it = tokens_.begin(); it != tokens_.end();) {
    if (it->second.expireAt <= now) it = tokens_.erase(it);
    else ++it;
  }
}

} // namespace im