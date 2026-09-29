#pragma once

#include "mysql_client.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace im {

// 群组/成员/水位线的 MySQL 存储。
// 仿 ContactRepository：单连接 + 内部互斥锁串行化 DB 访问。
// isMember 用内存缓存（members_ 集合，仅 left_ts==0 的现成员）加速发送热路径，
// 启动全量加载；加人/退群时同步刷新缓存。
class GroupRepository {
public:
  GroupRepository(const std::string& host, unsigned port, const std::string& user,
                  const std::string& password, const std::string& db);

  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  // ---- 群 ----
  // 建群，返回 groupId（>0），失败返回 0。
  int64_t createGroup(const std::string& name, int64_t ownerId, int64_t now);
  bool groupExists(int64_t groupId);
  // 查询群信息，找到返回 true（写出 name/ownerId；后三个可选，传 nullptr 则不写出）。
  bool getGroup(int64_t groupId, std::string* name, int64_t* ownerId,
                std::string* announcement = nullptr, int64_t* announceTs = nullptr,
                bool* dissolved = nullptr);
  // 转让群主 / 解散 / 改名 / 发布公告（空 text=清空）。成功返回 true。
  bool setOwner(int64_t groupId, int64_t newOwnerId);
  bool dissolveGroup(int64_t groupId, int64_t now);
  bool renameGroup(int64_t groupId, const std::string& name);
  bool setAnnouncement(int64_t groupId, const std::string& text, int64_t now);

  // ---- 成员 ----
  bool isMember(int64_t groupId, int64_t userId);  // 现成员（走缓存）
  // 成员关系（含退群）：有行返回 true（写出 joinedTs/leftTs，leftTs==0 表示在群）。
  bool memberPeriod(int64_t groupId, int64_t userId, int64_t* joinedTs, int64_t* leftTs);
  // 加人（新加或重入：重入时 joined_ts=now、left_ts=0）。成功返回 true。
  bool addMember(int64_t groupId, int64_t userId, int64_t now);
  // 退群（left_ts=now，行保留）。成功返回 true。
  bool removeMember(int64_t groupId, int64_t userId, int64_t now);
  size_t activeCount(int64_t groupId);                    // 现成员数
  std::vector<int64_t> listMemberIds(int64_t groupId);    // 现成员 userId 列表
  std::vector<int64_t> listUserGroups(int64_t userId);    // 某用户所在现群 groupId 列表

  // ---- 水位线 ----
  int64_t getWatermark(int64_t groupId, int64_t userId);
  void setWatermark(int64_t groupId, int64_t userId, int64_t ts);

private:
  void loadMembersCache();
  // 锁内全量查询现成员关系到 out；查询失败返回 false（不动 out）
  bool fetchActiveMembers(std::unordered_set<std::string>* out);

  MySqlClient client_;
  std::mutex mutex_;  // 串行化 DB 访问 + 缓存读写
  // 现成员关系缓存；isMember 查 "gid|uid"
  std::unordered_set<std::string> members_;  // 键格式 "gid|uid"

public:
  // ---- 缓存低频比对（防绕过服务端直改库导致缓存漂移）----
  // 库内现成员（left_ts=0）总行数；查询失败返回 false（调用方跳过本轮比对）
  bool dbMemberCount(size_t* out);
  size_t cacheMemberCount();  // 内存缓存条目数（锁内）
  // 全量重建缓存：先查到临时集合、成功才换入（失败保持旧缓存，不清空）
  void reloadMembersCache();
};

}  // namespace im
