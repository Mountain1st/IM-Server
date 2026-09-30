#pragma once

#include "mysql_client.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

namespace im {

// 拉黑（黑名单）存储：单向行 (user_id, blocked_id)，双向断联靠查询时
// 配对检查。仿 ContactRepository：单连接 + 内部互斥锁、内存缓存
// （键 "拉黑方|被拉黑方"）、启动全量加载 + 增删同步刷缓存 +
// syncCaches 计数比对自愈（第四个纳入自愈体系的缓存，2026-10-01）。
// 热路径 isBlockedPair 两次内存查找零 DB。
class BlockRepository {
public:
  BlockRepository(const std::string& host, unsigned port, const std::string& user,
                  const std::string& password, const std::string& db);

  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  // ---- 查询（走缓存，热路径）----
  // a 与 b 之间是否存在任一方向的拉黑（双向断联判定）
  bool isBlockedPair(int64_t a, int64_t b);
  // a 是否拉黑了 b（方向性判定：拉黑方发消息回「你已拉黑对方」用）
  bool isOneWayBlockedBy(int64_t a, int64_t b);

  // ---- 写入 ----
  // 拉黑。返回：0=成功；1=已拉黑（409）；2=DB 错误
  int addBlock(int64_t userId, int64_t blockedId, int64_t now);
  // 解除。返回：0=成功；1=未拉黑（409）；2=DB 错误
  int removeBlock(int64_t userId, int64_t blockedId);
  // 某用户的拉黑列表：返回 (blockedId, createdTs) 列表
  std::vector<std::pair<int64_t, int64_t>> listBlocked(int64_t userId);

  // ---- 缓存低频比对（syncCaches 第四段；防绕库直改漂移）----
  // 库内 blocks 总行数；查询失败返回 false（调用方跳过本轮比对）
  bool dbBlockCount(size_t* out);
  size_t cacheBlockCount();  // 内存缓存条目数（锁内）
  // 全量重建缓存：先查到临时集合、成功才换入（失败保持旧缓存，不清空）
  void reloadBlocksCache();

private:
  // 锁内全量查询拉黑关系到 out；查询失败返回 false（不动 out）
  bool fetchAllBlocks(std::unordered_set<std::string>* out);

  MySqlClient client_;
  std::mutex mutex_;  // 串行化 DB 访问 + 缓存读写
  // 单向拉黑关系缓存；键格式 "拉黑方|被拉黑方"
  std::unordered_set<std::string> blocks_;
};

} // namespace im
