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
// 密码派生参数：PBKDF2-HMAC-SHA256，10 万次迭代，输出 32 字节（256 位）
constexpr unsigned kPbkdf2Iterations = 100000;
constexpr size_t kPbkdf2DkLen = 32;
}

UserStore::UserStore(std::shared_ptr<UserRepository> repo)
  : repo_(std::move(repo)) {}

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
  // 慢哈希：PBKDF2-HMAC-SHA256，迭代 10 万次，抵御 GPU/ASIC 离线爆破
  return pbkdf2HmacSha256Hex(password, salt, kPbkdf2Iterations, kPbkdf2DkLen);
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
  std::string salt = genSalt();
  return repo_->insertUser(username, salt, hashPassword(salt, password), userId);
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

  int64_t id = 0;
  std::string salt, passwordHash;
  if (!repo_->fetchUser(username, &id, &salt, &passwordHash)) { // 用户不存在，统一按密码错误处理
    recordFail(username, now);
    return 403;
  }
  if (hashPassword(salt, password) != passwordHash) {
    recordFail(username, now);
    return 403;
  }
  loginGuards_.erase(username); // 登录成功，清空失败记录
  *token = genToken();
  tokens_[*token] = TokenEntry{id, now + kTokenTtl};
  *userId = id;
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
  int64_t uid = it->second.userId;
  std::string name;
  if (!repo_->fetchUserNameById(uid, &name)) return 401; // 用户已不存在
  *username = name;
  *userId = uid;
  return 0;
}

void UserStore::logout(const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  tokens_.erase(token);
}

bool UserStore::exists(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return repo_->userExists(userId);
}

} // namespace im