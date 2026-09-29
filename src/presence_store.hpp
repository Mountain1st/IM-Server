#pragma once

// 在线状态（Presence）存储抽象：登录连接数计数 + 在线查询。
// 双后端：内存（默认）与 Redis（IM_REDIS_ENABLED=1，供外部观察在线状态）。
// 由 OnlineManager 在 add/remove 会话时同步维护，业务侧（contact.list /
// group.members）经 isOnline 查询并随列表下发 online 字段。

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace im {

class PresenceStore {
public:
  virtual ~PresenceStore() = default;

  // 用户上线一个连接（计数 +1）。多端登录时计数累加。
  virtual void online(int64_t userId) = 0;
  // 用户断开一个连接（计数 -1，归零即离线）。
  virtual void offline(int64_t userId) = 0;
  // 该用户当前是否在线（至少一个存活连接）。
  virtual bool isOnline(int64_t userId) = 0;
  // 批量在线查询（列表接口用，避免 N+1 逐条往返）。
  // 返回与 userIds 等长的布尔向量；默认逐条实现，后端可覆写为批量协议（如 MGET）。
  virtual std::vector<bool> isOnlineBatch(const std::vector<int64_t>& userIds) {
    std::vector<bool> out;
    out.reserve(userIds.size());
    for (int64_t uid : userIds) out.push_back(isOnline(uid));
    return out;
  }
};

// 内存后端：userId -> 连接计数。单实例下的权威实现。
class MemoryPresenceStore : public PresenceStore {
public:
  void online(int64_t userId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++counts_[userId];
  }
  void offline(int64_t userId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counts_.find(userId);
    if (it == counts_.end()) return; // 防御：未上线却断开
    if (--it->second <= 0) counts_.erase(it);
  }
  bool isOnline(int64_t userId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counts_.find(userId);
    return it != counts_.end() && it->second > 0;
  }

private:
  std::mutex mutex_;
  std::unordered_map<int64_t, int64_t> counts_;
};

} // namespace im
