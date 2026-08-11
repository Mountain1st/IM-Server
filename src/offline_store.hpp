#pragma once

#include "message.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace im {

// 离线消息存储（内存版）：单聊目标不在线时暂存，登录后经 msg.offline 补发。
// 多线程访问，内部用互斥锁保护；重启清空（后续接 Redis 时替换）。
class OfflineStore {
public:
  // 入队一条离线消息（msg 为完整 msg.recv 的 data 部分）。超限时丢弃最旧一条。
  void push(int64_t userId, const json& msg);
  // 取出并清空该用户全部离线消息（登录补发后即删除）。
  std::vector<json> take(int64_t userId);

private:
  std::mutex mutex_;
  static constexpr size_t kMaxPerUser = 100; // 每用户离线消息上限，防内存膨胀
  std::unordered_map<int64_t, std::deque<json>> store_; // userId -> 消息队列
};

} // namespace im
