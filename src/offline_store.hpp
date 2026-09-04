#pragma once

#include "message.hpp"
#include "server_config.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace im {

// 离线消息存储（内存版）：单聊目标不在线时暂存，登录后经 msg.offline 补发。
// 可靠性语义（P0-7）：
// - peek() 非破坏性：补发中断不丢失，重登可重新补发；
// - 消息保留至 TTL 到期自动清理，超每用户上限时丢弃最旧；
// - 消息带全局 messageId（由发送方在 push 前写入 msg），供客户端去重。
// 多线程访问，内部用互斥锁保护；重启清空（后续接 Redis 时替换）。
class OfflineStore {
public:
  // 入队一条离线消息（msg 为完整 msg.recv 的 data 部分，须含 messageId）。
  // 超限丢最旧，TTL 过期自动清理。返回是否入队成功。
  bool push(int64_t userId, const json& msg);
  // 非破坏性取出：返回该用户全部离线消息，不移除（重登仍可再次补发）。
  std::vector<json> peek(int64_t userId);

private:
  struct Entry {
    json msg;
    std::chrono::steady_clock::time_point storedAt;
  };
  void cleanExpiredLocked();

  std::mutex mutex_;
  std::unordered_map<int64_t, std::deque<Entry>> store_; // userId -> 消息队列
};

} // namespace im