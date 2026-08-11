#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace im {

// 消息幂等去重（内存版）：按发送方 userId 缓存最近处理过的 clientMsgId，
// 防止客户端网络重发导致同一消息被重复投递。多线程访问，内部互斥锁保护。
class MsgDedup {
public:
  // 检查并记录一条 clientMsgId。
  // 返回 true  表示该 clientMsgId 在窗口内已处理过（重复，应跳过投递）；
  // 返回 false 表示首次见到（已记录）。
  // clientMsgId 为空时不参与去重，恒返回 false。
  bool checkAndRemember(int64_t userId, const std::string& clientMsgId);

private:
  // 内部记录：clientMsgId -> 首次见到时间
  struct Entry {
    std::chrono::steady_clock::time_point seenAt;
  };
  // 每个用户最多缓存的去重 id 数，超出时淘汰最旧，防内存膨胀
  static constexpr size_t kMaxPerUser = 1024;
  // 去重窗口时长
  static constexpr std::chrono::minutes kWindow = std::chrono::minutes(5);

  std::mutex mutex_;
  std::unordered_map<int64_t, std::unordered_map<std::string, Entry>> map_; // userId -> id 表
};

} // namespace im
