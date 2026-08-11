#include "user_store.hpp"

#include "util.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace im {

namespace {
// token 有效期：7 天
constexpr std::chrono::hours kTokenTtl(7 * 24);
// 周期性清理过期 token 的阈值（每次 login 时顺带检查）
constexpr size_t kTokenCleanupThreshold = 1024;
// 登录限流：窗口内失败次数上限与窗口时长
constexpr int kMaxLoginFailures = 5;
constexpr std::chrono::seconds kLockWindow(60);
constexpr size_t kLoginGuardLimit = 1024;
}

std::string UserStore::genSalt() {
  std::random_device rd;
  std::uniform_int_distribution<unsigned> dist(0, 255);
  std::ostringstream oss;
  for (int i = 0; i < 16; ++i) {
    oss << std::hex << std::setw(2) << std::setfill('0') << dist(rd);
  }
  return oss.str();
}

std::string UserStore::hashPassword(const std::string& salt, const std::string& password) {
  return sha1Hex(salt + ":" + password);
}

std::string UserStore::genToken() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::ostringstream oss;
  // 128 位随机数，避免可预测性；不再拼用户名（无助于防碰撞）
  oss << std::hex << gen() << gen();
  return sha1Hex(oss.str());
}

int UserStore::registerUser(const std::string& username, const std::string& password, int64_t* userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (byName_.count(username)) return 409; // 用户名已存在
  int64_t id = nextId_++;
  std::string salt = genSalt();
  users_.emplace(id, User{id, username, salt, hashPassword(salt, password)});
  byName_[username] = id;
  *userId = id;
  return 0;
}

int UserStore::login(const std::string& username, const std::string& password, int64_t* userId, std::string* token) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();

  // 登录限流：窗口内失败次数已达阈值则拒绝（返回 429），窗口过期后重置
  auto guardIt = loginGuards_.find(username);
  if (guardIt != loginGuards_.end()) {
    if (now - guardIt->second.windowStart >= kLockWindow) {
      loginGuards_.erase(guardIt);
    } else if (guardIt->second.failures >= kMaxLoginFailures) {
      return 429; // 尝试过于频繁，稍后再试
    }
  }

  auto it = byName_.find(username);
  if (it == byName_.end()) { // 用户不存在，统一按密码错误处理
    recordFail(username, now);
    return 403;
  }
  const User& u = users_.at(it->second);
  if (u.passwordHash != hashPassword(u.salt, password)) {
    recordFail(username, now);
    return 403;
  }
  loginGuards_.erase(username); // 登录成功，清空失败记录
  *token = genToken();
  tokens_[*token] = TokenEntry{u.id, now + kTokenTtl};
  *userId = u.id;
  // 顺带清理过期 token，防止表无限膨胀
  if (tokens_.size() > kTokenCleanupThreshold) {
    for (auto t = tokens_.begin(); t != tokens_.end();) {
      if (t->second.expireAt <= now) t = tokens_.erase(t);
      else ++t;
    }
  }
  return 0;
}

void UserStore::recordFail(const std::string& username, std::chrono::steady_clock::time_point now) {
  auto& g = loginGuards_[username];
  if (g.failures == 0) g.windowStart = now; // 窗口起点以首次失败计
  ++g.failures;
  // 防表无限膨胀：超限时先清理已过期条目；若仍超限，
  // 则强淘汰窗口最早（最旧）的条目（跳过当前用户名，
  // 防止攻击者用海量新用户名把目标账号的失败计数挤掉，绕过限流）。
  if (loginGuards_.size() > kLoginGuardLimit) {
    for (auto it = loginGuards_.begin(); it != loginGuards_.end();) {
      if (now - it->second.windowStart >= kLockWindow) it = loginGuards_.erase(it);
      else ++it;
    }
    while (loginGuards_.size() > kLoginGuardLimit) {
      auto oldest = loginGuards_.end();
      for (auto it = loginGuards_.begin(); it != loginGuards_.end(); ++it) {
        if (it->first == username) continue;
        if (oldest == loginGuards_.end() ||
            it->second.windowStart < oldest->second.windowStart) {
          oldest = it;
        }
      }
      if (oldest == loginGuards_.end()) break; // 只剩当前条目，无法再淘汰
      loginGuards_.erase(oldest);
    }
  }
}

int UserStore::verifyToken(const std::string& token, int64_t* userId, std::string* username) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  auto it = tokens_.find(token);
  if (it == tokens_.end() || it->second.expireAt <= now) {
    if (it != tokens_.end()) tokens_.erase(it); // 顺手清掉过期 token
    return 401; // token 无效或已过期
  }
  auto u = users_.find(it->second.userId);
  if (u == users_.end()) return 401;
  *userId = u->second.id;
  *username = u->second.username;
  return 0;
}

void UserStore::logout(const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  tokens_.erase(token);
}

bool UserStore::exists(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return users_.count(userId) != 0;
}

} // namespace im
