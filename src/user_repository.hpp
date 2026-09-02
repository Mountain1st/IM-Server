#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace im {

// 用户存储接口：UserStore（负责 token 签发/限流/密码哈希）把账号的增查
// 持久化委托给本接口。当前唯一实现为 MySQL（MySqlUserRepository）。
// 约定：调用方处于 UserStore 的互斥锁内，本接口方法按单线程访问设计。
class UserRepository {
public:
  virtual ~UserRepository() = default;

  // 插入新用户。成功返回 0 并写出新用户 id；用户名已存在返回 409。
  virtual int insertUser(const std::string& username, const std::string& salt,
                         const std::string& passwordHash, int64_t* userId) = 0;
  // 按用户名查询用户（用于登录校验）。找到返回 true 并写出 id/salt/hash。
  virtual bool fetchUser(const std::string& username, int64_t* userId,
                         std::string* salt, std::string* passwordHash) = 0;
  // 用户 id 是否存在（msg.send 目标校验）。
  virtual bool userExists(int64_t userId) = 0;
  // 按用户 id 查询用户名（token 校验回执用）。找到返回 true 并写出 username。
  virtual bool fetchUserNameById(int64_t userId, std::string* username) = 0;
};

} // namespace im