#pragma once

#include "mysql_client.hpp"
#include "user_repository.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

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

  int insertUser(const std::string& username, const std::string& salt,
                 const std::string& passwordHash, int64_t* userId) override;
  bool fetchUser(const std::string& username, int64_t* userId,
                 std::string* salt, std::string* passwordHash) override;
  bool userExists(int64_t userId) override;
  bool fetchUserNameById(int64_t userId, std::string* username) override;

private:
  void loadIdCache();

  MySqlClient client_;
  // 已知 userId -> username 缓存（启动全量加载 + 注册时追加）。
  // userExists() 与 token 回执的 username 查询都走缓存，避免热路径打数据库。
  // 本服务不提供删号，缓存不会过期；若外部直接改库则需重启同步。
  std::unordered_map<int64_t, std::string> ids_;
};

} // namespace im