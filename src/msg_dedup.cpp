#include "msg_dedup.hpp"

namespace im {

namespace {
constexpr size_t kPerUserCleanupThreshold = 512; // 每次超过此数顺带清理过期
}

bool MsgDedup::checkAndRemember(int64_t userId, const std::string& clientMsgId) {
  if (clientMsgId.empty()) return false; // 无 id 不参与去重
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();

  auto& ids = map_[userId];
  auto it = ids.find(clientMsgId);
  if (it != ids.end() && now - it->second.seenAt <= kWindow) {
    return true; // 窗口内重复
  }
  // 首次见到（或窗口已过期被覆盖）：记录并返回 false
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
  return false;
}

} // namespace im
