#pragma once

#include "server_config.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace im {

// 消息幂等去重存储接口：按发送方 userId 缓存最近处理过的 clientMsgId，
// 防止客户端网络重发导致同一消息被重复投递。双后端：
// - MemoryMsgDedup（默认）
// - RedisMsgDedup（SET NX EX，跨重启/多实例共享）
// 实现须自身保证线程安全。
class MsgDedupStore {
public:
  virtual ~MsgDedupStore() = default;

  // 只查询不记录：返回 true 表示该 clientMsgId 在窗口内已处理过（重复）。
  // 与 remember() 分离，使调用方能按「限流 → 幂等查询 → 投递 → 记录」的顺序
  // 处理，避免被限流丢弃的请求污染幂等记录（否则重试会误回成功）。
  // clientMsgId 为空时恒返回 false（不参与去重）。
  virtual bool isDuplicate(int64_t userId, const std::string& clientMsgId) = 0;
  // 记录一条 clientMsgId（在消息确认投递/入队之后调用）。空 id 不做任何事。
  virtual void remember(int64_t userId, const std::string& clientMsgId) = 0;
  // 兼容旧语义：检查并记录（仅测试或一次性迁移使用）。
  virtual bool checkAndRemember(int64_t userId, const std::string& clientMsgId) = 0;
};

// 内存实现（默认）：userId -> clientMsgId 表，含过期清理与上限淘汰。
class MemoryMsgDedup : public MsgDedupStore {
public:
  bool isDuplicate(int64_t userId, const std::string& clientMsgId) override;
  void remember(int64_t userId, const std::string& clientMsgId) override;
  bool checkAndRemember(int64_t userId, const std::string& clientMsgId) override;

private:
  struct Entry {
    std::chrono::steady_clock::time_point seenAt;
  };
  // 每个用户最多缓存的去重 id 数，超出时淘汰最旧，防内存膨胀
  static constexpr size_t kMaxPerUser = config::kDedupMaxPerUser;
  // 去重窗口时长
  static constexpr std::chrono::minutes kWindow = config::kDedupWindow;

  std::mutex mutex_;
  std::unordered_map<int64_t, std::unordered_map<std::string, Entry>> map_; // userId -> id 表
};

} // namespace im