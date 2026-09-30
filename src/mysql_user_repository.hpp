#pragma once

#include "mysql_client.hpp"
#include "user_repository.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace im {

// MySQL 版用户存储：账号增查落库（users 表）。
// - 全部使用 prepared statement，防 SQL 注入；
// - 启动时加载全部 userId 到内存缓存，供 msg.send 的 exists() 热路径使用，
//   避免每条消息打一次数据库；
// - 调用方（UserStore）持锁串行访问，本类不额外加锁。
class MySqlUserRepository : public UserRepository {
public:
  MySqlUserRepository(const std::string& host, unsigned port, const std::string& user,
                      const std::string& password, const std::string& db);

  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  int insertUser(const std::string& account, const std::string& username,
                 const std::string& salt, const std::string& passwordHash,
                 int64_t* userId) override;
  int fetchUser(const std::string& account, int64_t* userId,
                std::string* username, std::string* salt,
                std::string* passwordHash) override;
  int fetchUserInfo(const std::string& account, int64_t* userId,
                    std::string* username) override;
  bool userExists(int64_t userId) override;
  bool fetchUserNameById(int64_t userId, std::string* username) override;
  bool fetchAccountById(int64_t userId, std::string* account) override;
  int fetchSettings(int64_t userId, int* allowTempChat, int* searchable,
                    int* allowAdd) override;
  int updateSettings(int64_t userId, int allowTempChat, int searchable,
                     int allowAdd) override;
  void searchUsers(const std::string& keyword, int64_t excludeUserId, int limit,
                   std::vector<std::pair<std::string, std::string>>* out) override;

private:
  void loadIdCache();
  // 锁内全量查询用户 id/账号/昵称到临时结构；查询失败返回 false（不动 out）
  bool fetchAllUsers(std::unordered_map<int64_t, std::string>* ids,
                     std::unordered_map<int64_t, std::string>* idAccounts);

  MySqlClient client_;
  // 串行化 DB 访问 + 缓存读写。UserStore 门面锁只覆盖工作线程侧；
  // syncCaches（清理线程）绕过门面直调 sync 接口，故仓储自身有锁
  // （与 Contact/GroupRepository 同构；两层锁顺序恒为 UserStore→repo，
  // 无反向调用，无死锁）。2026-09-30 起本类按"自带锁、可跨线程"设计
  std::mutex mutex_;
  // 已知 userId -> 昵称(username) 缓存（启动全量加载 + 注册时追加）。
  // userExists()、token 回执与 msg.recv 的 fromNick 都走缓存，避免热路径打数据库。
  // 本服务不提供删号，缓存不会过期；绕库直改的自愈见下方 sync 接口。
  std::unordered_map<int64_t, std::string> ids_;
  // 已知 userId -> 登录账号(account) 缓存（启动全量加载 + 注册时追加）。
  // msg.recv/msg.offline/chat.history_res 的 from 返回 account，需要反查。
  std::unordered_map<int64_t, std::string> idAccounts_;

public:
  // ---- 缓存低频比对（防绕库直改导致漂移；与 Contact/Group 同构）----
  // 库内 users 总行数；查询失败返回 false（调用方跳过本轮比对）
  bool dbUserCount(size_t* out) override;
  size_t cacheUserCount() override;  // 内存缓存条目数（锁内）
  // 全量重建：先查到临时结构、成功才换入（失败保持旧缓存，不清空）
  void reloadIdCache() override;
};

} // namespace im