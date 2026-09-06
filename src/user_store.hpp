#pragma once

#include "token_store.hpp"
#include "user_repository.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace im {

// MySQL 连接配置。缺省值（IMS_reactor_db / IMS_jack / 520010）由 IM_MYSQL_*
// 环境变量覆盖；连接失败时启动报错退出。
struct UserStoreConfig {
  std::string mysqlHost = "127.0.0.1";
  unsigned mysqlPort = 3306;
  std::string mysqlUser = "IMS_jack";
  std::string mysqlPassword = "520010";
  std::string mysqlDb = "IMS_reactor_db";
};

// 账号存储：负责注册、登录鉴权、token 发放/校验/登出、登录失败限流。
// 用户模型：userId(id) 为唯一标识（消息收发目标），account 为登录账号，
// username 为昵称。密码哈希（PBKDF2）与登录限流在本类内存中实现；
// 用户的增查持久化委托给 UserRepository（MySQL）；
// token 的存储委托给 TokenStore（内存默认，Redis 可选）。
// 多个工作线程会并发访问，内部统一用互斥锁保护。
class UserStore {
public:
  // 必须注入存储后端（MySQL + token 存储）；连接失败由上层在启动前报错退出。
  explicit UserStore(std::shared_ptr<UserRepository> repo,
                     std::shared_ptr<TokenStore> tokenStore);

  // 注册新账号。成功返回 0 并写出新用户 id；账号已存在返回 409。
  int registerUser(const std::string& account, const std::string& password,
                   const std::string& username, int64_t* userId);
  // 登录：校验账号密码。成功返回 0 并写出 userId/昵称/新 token；失败返回 403。
  int login(const std::string& account, const std::string& password,
            int64_t* userId, std::string* username, std::string* token);
  // 校验 token 是否有效（存在且未过期）。成功返回 0 并写出 userId/昵称；无效返回 401。
  int verifyToken(const std::string& token, int64_t* userId, std::string* username);
  // 登出：使 token 失效（无副作用）。
  void logout(const std::string& token);
  // 用户是否存在（用于 msg.send 的目标校验）。
  bool exists(int64_t userId);
  // 按登录账号查询用户信息（新建对话用）。返回：0=找到(写出 id/昵称)、1=不存在、2=数据库错误。
  int lookupAccount(const std::string& account, int64_t* userId, std::string* username);
  // 查询用户昵称（msg.recv 的 fromNick）。不存在返回 false。
  bool nickname(int64_t userId, std::string* username);
  // 查询用户登录账号（S2C 消息 from 用 account）。不存在返回 false。
  bool accountOf(int64_t userId, std::string* account);
  // 隐私设置转发（见 UserRepository）
  int fetchSettings(int64_t userId, int* allowTempChat, int* searchable, int* allowAdd);
  int updateSettings(int64_t userId, int allowTempChat, int searchable, int allowAdd);
  void searchUsers(const std::string& keyword, int64_t excludeUserId, int limit,
                   std::vector<std::pair<std::string, std::string>>* out);

private:
  // 随机盐 + 密码哈希（PBKDF2-HMAC-SHA256 慢哈希，见 user_store.cpp）
  std::string genSalt();
  std::string hashPassword(const std::string& salt, const std::string& password);
  // 生成登录 token（随机种子，SHA-1 摘要）
  std::string genToken();
  // 记录一次登录失败（含窗口初始化与防表膨胀清理），供 login 内部调用
  void recordFail(const std::string& account, std::chrono::steady_clock::time_point now);

  std::mutex mutex_;
  std::shared_ptr<UserRepository> repo_;
  std::shared_ptr<TokenStore> tokenStore_; // token 存储（内存默认 / Redis 可选）
  // 登录失败限流记录：窗口内失败达到阈值则暂时拒绝登录（防密码爆破）
  struct LoginGuard {
    int failures = 0;
    std::chrono::steady_clock::time_point windowStart;
  };
  std::unordered_map<std::string, LoginGuard> loginGuards_;    // account -> 失败记录
  struct Settings {
    int allowTempChat = 1;
    int searchable = 1;
    int allowAdd = 1;
  };
  std::unordered_map<int64_t, Settings> settingsCache_;  // userId -> 设置缓存
};

} // namespace im