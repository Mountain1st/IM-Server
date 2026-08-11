#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace im {

// 账号存储（内存版）：负责注册、登录鉴权、token 发放/校验/登出。
// 多个工作线程会并发访问，内部统一用互斥锁保护。
// 注意：内存版重启即清空，后续接 MySQL 时替换本类。
class UserStore {
public:
  // 注册新账号。成功返回 0 并写出新用户 id；用户名已存在返回 409。
  int registerUser(const std::string& username, const std::string& password, int64_t* userId);
  // 登录：校验用户名密码。成功返回 0 并写出 userId 与新 token；失败返回 403。
  int login(const std::string& username, const std::string& password, int64_t* userId, std::string* token);
  // 校验 token 是否有效（存在且未过期）。成功返回 0 并写出 userId/username；无效返回 401。
  int verifyToken(const std::string& token, int64_t* userId, std::string* username);
  // 登出：使 token 失效（无副作用）。
  void logout(const std::string& token);
  // 用户是否存在（用于 msg.send 的目标校验）。
  bool exists(int64_t userId);

private:
  // 随机盐 + 密码哈希（演示级；正式版应换 bcrypt 等慢哈希算法）
  std::string genSalt();
  std::string hashPassword(const std::string& salt, const std::string& password);
  // 生成登录 token（随机种子，SHA-1 摘要）
  std::string genToken();
  // 记录一次登录失败（含窗口初始化与防表膨胀清理），供 login 内部调用
  void recordFail(const std::string& username, std::chrono::steady_clock::time_point now);

  std::mutex mutex_;
  int64_t nextId_ = 1;
  struct User {
    int64_t id;
    std::string username;
    std::string salt;
    std::string passwordHash;
  };
  struct TokenEntry {
    int64_t userId;
    std::chrono::steady_clock::time_point expireAt;
  };
  // 登录失败限流记录：窗口内失败达到阈值则暂时拒绝登录（防密码爆破）
  struct LoginGuard {
    int failures = 0;
    std::chrono::steady_clock::time_point windowStart;
  };
  std::unordered_map<int64_t, User> users_;         // id -> 用户
  std::unordered_map<std::string, int64_t> byName_; // username -> id
  std::unordered_map<std::string, TokenEntry> tokens_; // token -> 用户+过期时间
  std::unordered_map<std::string, LoginGuard> loginGuards_; // username -> 失败记录
};

} // namespace im
