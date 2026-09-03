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

  // 插入新用户（account 为登录账号，username 为昵称）。
  // 成功返回 0 并写出新用户 id；账号已存在返回 409。
  virtual int insertUser(const std::string& account, const std::string& username,
                         const std::string& salt, const std::string& passwordHash,
                         int64_t* userId) = 0;
  // 按登录账号查询用户（用于登录校验）。
  // 返回：0=找到（写出 id/昵称/salt/hash）；1=账号不存在；2=数据库错误。
  virtual int fetchUser(const std::string& account, int64_t* userId,
                        std::string* username, std::string* salt,
                        std::string* passwordHash) = 0;
  // 按登录账号查询用户信息（新建对话 chat.create 用）。
  // 返回：0=找到（写出 id/昵称）；1=账号不存在；2=数据库错误。
  virtual int fetchUserInfo(const std::string& account, int64_t* userId,
                            std::string* username) = 0;
  // 用户 id 是否存在（msg.send 目标校验）。
  virtual bool userExists(int64_t userId) = 0;
  // 按用户 id 查询昵称（token 回执、消息 fromNick）。找到返回 true 并写出昵称。
  virtual bool fetchUserNameById(int64_t userId, std::string* username) = 0;
};

} // namespace im