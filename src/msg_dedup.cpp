#include "msg_dedup.hpp"

namespace im {

namespace {
constexpr size_t kPerUserCleanupThreshold = 512; // 每次超过此数顺带清理过期
}

bool MsgDedup::isDuplicate(int64_t userId, const std::string& clientMsgId) {
  if (clientMsgId.empty()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  auto it = map_.find(userId);
  if (it == map_.end()) return false;
  auto jt = it->second.find(clientMsgId);
  return jt != it->second.end() && now - jt->second.seenAt <= kWindow;
}

void MsgDedup::remember(int64_t userId, const std::string& clientMsgId) {
  if (clientMsgId.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  auto& ids = map_[userId];
  ids[clientMsgId] = Entry{now};

  // 防内存膨胀：超限时先清过期，仍超限则强淘汰最旧条目
  if (ids.size() > kPerUserCleanupThreshold) {
    for (auto i = ids.begin(); i != ids.end();) {
      if (now - i->second.seenAt > kWindow) i = ids.erase(i);
      else ++i;
    }
    while (ids.size() > kMaxPerUser) {
      auto oldest = ids.begin();
      for (auto i = ids.begin(); i != ids.end(); ++i) {
        if (i->second.seenAt < oldest->second.seenAt) oldest = i;
      }
      ids.erase(oldest);
    }
  }
}

bool MsgDedup::checkAndRemember(int64_t userId, const std::string& clientMsgId) {
  bool dup = isDuplicate(userId, clientMsgId);
  remember(userId, clientMsgId);
  return dup;
}

} // namespace im
