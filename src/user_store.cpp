// ============================================================
// UserStore：账号体系门面（仓储 + token 存储 + 登录限流守卫）
// - registerUser/login/logout：透传仓储并维护 token（7 天有效，
//   摘要化存储：库中只存 SHA256(token)，泄露不可逆推 token）
// - 登录失败限流（guard_）：内存计数，连续失败 5 次锁 60 秒；
//   判定在 DB 查询之前（防"查库洗表"重置计数，见
//   tests/login_guard_test.cpp）。已知限制：**计数在内存，进程重启
//   即清零**——重启洗白需攻击者具备重启能力（彼时已沦陷），
//   防护目标是在线爆破，跨重启持久化不做（见协议 §11.3 取舍）
// 线程：工作线程池调用；内部有锁
// ============================================================
#include "user_store.hpp"

#include "server_config.hpp"
#include "util.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace im {

namespace {
// token 有效期：7 天（取自集中配置）
constexpr std::chrono::hours kTokenTtl(config::kTokenTtl);
// 登录限流：窗口内失败次数上限与窗口时长
constexpr int kMaxLoginFailures = config::kMaxLoginFailures;
constexpr std::chrono::seconds kLockWindow(config::kLockWindow);
constexpr size_t kLoginGuardLimit = 1024;
// 密码派生参数：PBKDF2-HMAC-SHA256，10 万次迭代，输出 32 字节（256 位）
constexpr unsigned kPbkdf2Iterations = 100000;
constexpr size_t kPbkdf2DkLen = 32;
}

UserStore::UserStore(std::shared_ptr<UserRepository> repo,
                     std::shared_ptr<TokenStore> tokenStore)
  : repo_(std::move(repo)), tokenStore_(std::move(tokenStore)) {}

UserStore::Pbkdf2Gate::Pbkdf2Gate(UserStore* s) : store_(s) {
  // 满则排队（futex 等待不占 CPU）；进入即计数
  std::unique_lock<std::mutex> lk(store_->pbkdf2Mutex_);
  store_->pbkdf2Cv_.wait(lk, [s = store_] { return s->pbkdf2Active_ < config::kMaxConcurrentPbkdf2; });
  ++store_->pbkdf2Active_;
}

UserStore::Pbkdf2Gate::~Pbkdf2Gate() {
  {
    std::lock_guard<std::mutex> lk(store_->pbkdf2Mutex_);
    --store_->pbkdf2Active_;
  }
  store_->pbkdf2Cv_.notify_one();
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
  // 慢哈希：PBKDF2-HMAC-SHA256，迭代 10 万次，抵御 GPU/ASIC 离线爆破
  return pbkdf2HmacSha256Hex(password, salt, kPbkdf2Iterations, kPbkdf2DkLen);
}

std::string UserStore::genToken() {
  // 直接取 random_device（Linux 下通常是 CSPRNG）的 256 位熵，避免
  // mt19937_64 单 32 位种子导致的 token 可枚举问题。
  std::random_device rd;
  std::ostringstream oss;
  for (int i = 0; i < 32; ++i) {
    oss << std::hex << std::setw(2) << std::setfill('0') << (rd() & 0xFF);
  }
  return oss.str();
}

int UserStore::registerUser(const std::string& account, const std::string& password,
                            const std::string& username, int64_t* userId) {
  // salt/哈希生成与 PBKDF2 均在锁外（salt 随机、哈希无共享状态）；
  // PBKDF2 同时受并发闸约束（A1）。mutex_ 只保护 ids_ 语义上不需要——
  // insertUser 内部（repo）自带锁维护缓存
  std::string salt = genSalt();
  std::string hash;
  {
    Pbkdf2Gate gate(this);
    hash = hashPassword(salt, password);
  }
  return repo_->insertUser(account, username, salt, hash, userId);
}

int UserStore::login(const std::string& account, const std::string& password,
                     int64_t* userId, std::string* username, std::string* token) {
  // 三段式拆锁（2026-09-30 自检三轮 B2）：PBKDF2 约 50-100ms，此前全程
  // 持 mutex_ 会阻塞消息热路径（lookupAccount/fetchSettings 共用此锁），
  // 5 个并发登录即可让全部消息投递停顿半秒。现改为：
  //   段1 锁内：guard 判定（429 或放行）
  //   段2 锁外：查库 + PBKDF2（重活）
  //   段3 锁内：失败记账 / 成功清 guard；token 签发（tokenStore 自带锁）
  // 锁语义：loginGuards_ 只在本锁内读写；并发同账号登录在 guard 上仅
  // 存在判定交错（双方都过判定、各自 PBKDF2）——最终记账原子，可接受
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto guardIt = loginGuards_.find(account);
    if (guardIt != loginGuards_.end()) {
      if (now - guardIt->second.windowStart >= kLockWindow) {
        loginGuards_.erase(guardIt);
      } else if (guardIt->second.failures >= kMaxLoginFailures) {
        return 429; // 尝试过于频繁，稍后再试
      }
    }
  }

  // 段2：锁外查库与慢哈希（repo/tokenStore 均自带锁，无需本锁保护）
  int64_t id = 0;
  std::string name, salt, passwordHash;
  int frc = repo_->fetchUser(account, &id, &name, &salt, &passwordHash);
  if (frc == 2) return 500; // 数据库错误，不能误报为密码错误
  if (frc != 0) { // 账号不存在，统一按密码错误处理
    std::lock_guard<std::mutex> lock(mutex_);
    recordFail(account, now);
    return 403;
  }
  {
    Pbkdf2Gate gate(this);
    if (hashPassword(salt, password) != passwordHash) {
      std::lock_guard<std::mutex> lock(mutex_);
      recordFail(account, now);
      return 403;
    }
  }
  // 段3：成功清失败记录（锁内），签发 token（tokenStore 自带锁，锁外）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loginGuards_.erase(account);
  }
  *token = genToken();
  // 只保存 token 的 SHA-256 摘要：内存/Redis 均不落明文 token（P0-5）
  tokenStore_->put(sha256Hex(*token), id, kTokenTtl);
  *userId = id;
  *username = name;
  return 0;
}

void UserStore::recordFail(const std::string& account, std::chrono::steady_clock::time_point now) {
  auto& g = loginGuards_[account];
  if (g.failures == 0) g.windowStart = now; // 窗口起点以首次失败计
  ++g.failures;
  // 防表无限膨胀：超限时先清理已过期条目；若仍超限，
  // 则强淘汰窗口最早（最旧）的条目。跳过当前账号与仍被锁定的账号：
  // 被锁定账号的计数是限流依据，淘汰它等于给攻击者清零（洗表绕过）。
  if (loginGuards_.size() > kLoginGuardLimit) {
    for (auto it = loginGuards_.begin(); it != loginGuards_.end();) {
      if (now - it->second.windowStart >= kLockWindow) it = loginGuards_.erase(it);
      else ++it;
    }
    auto locked = [&](const LoginGuard& e) {
      return e.failures >= kMaxLoginFailures && now - e.windowStart < kLockWindow;
    };
    while (loginGuards_.size() > kLoginGuardLimit) {
      auto oldest = loginGuards_.end();
      for (auto it = loginGuards_.begin(); it != loginGuards_.end(); ++it) {
        if (it->first == account) continue;
        if (locked(it->second)) continue; // 被锁定账号不可淘汰
        if (oldest == loginGuards_.end() ||
            it->second.windowStart < oldest->second.windowStart) {
          oldest = it;
        }
      }
      if (oldest == loginGuards_.end()) break; // 无可淘汰条目
      loginGuards_.erase(oldest);
    }
    // 仍超限说明表内全是被锁定账号：丢弃当前条目，表不再增长。
    // 此时新账号的失败不被记录（极端压力下对新键 fail-open），
    // 但已锁定账号的保护不受影响。
    if (loginGuards_.size() > kLoginGuardLimit) loginGuards_.erase(account);
  }
}

int UserStore::verifyToken(const std::string& token, int64_t* userId, std::string* username) {
  std::lock_guard<std::mutex> lock(mutex_);
  // token 摘要校验委托给 TokenStore（内存/Redis），过期自动失效
  if (tokenStore_->get(sha256Hex(token), userId) != 0) {
    return 401; // token 无效或已过期
  }
  std::string name;
  if (!repo_->fetchUserNameById(*userId, &name)) return 401; // 用户已不存在
  *username = name;
  return 0;
}

void UserStore::logout(const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  tokenStore_->remove(sha256Hex(token));
}

bool UserStore::exists(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return repo_->userExists(userId);
}

int UserStore::lookupAccount(const std::string& account, int64_t* userId, std::string* username) {
  std::lock_guard<std::mutex> lock(mutex_);
  return repo_->fetchUserInfo(account, userId, username);
}

bool UserStore::dbUserCacheCount(size_t* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  return repo_->dbUserCount(out);
}

size_t UserStore::cacheUserCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return repo_->cacheUserCount();
}

void UserStore::reloadUserCache() {
  std::lock_guard<std::mutex> lock(mutex_);
  repo_->reloadIdCache();
}

bool UserStore::nickname(int64_t userId, std::string* username) {
  std::lock_guard<std::mutex> lock(mutex_);
  return repo_->fetchUserNameById(userId, username);
}

bool UserStore::accountOf(int64_t userId, std::string* account) {
  std::lock_guard<std::mutex> lock(mutex_);
  return repo_->fetchAccountById(userId, account);
}

int UserStore::fetchSettings(int64_t userId, int* allowTempChat, int* searchable, int* allowAdd) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = settingsCache_.find(userId);
  if (it != settingsCache_.end()) {
    if (allowTempChat) *allowTempChat = it->second.allowTempChat;
    if (searchable) *searchable = it->second.searchable;
    if (allowAdd) *allowAdd = it->second.allowAdd;
    return 0;
  }
  Settings s;
  const int rc = repo_->fetchSettings(userId, &s.allowTempChat, &s.searchable, &s.allowAdd);
  if (rc != 0) return rc;
  settingsCache_[userId] = s;
  if (allowTempChat) *allowTempChat = s.allowTempChat;
  if (searchable) *searchable = s.searchable;
  if (allowAdd) *allowAdd = s.allowAdd;
  return 0;
}

int UserStore::updateSettings(int64_t userId, int allowTempChat, int searchable, int allowAdd) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int rc = repo_->updateSettings(userId, allowTempChat, searchable, allowAdd);
  if (rc == 0) settingsCache_.erase(userId);
  return rc;
}

void UserStore::searchUsers(const std::string& keyword, int64_t excludeUserId, int limit,
                            std::vector<std::pair<std::string, std::string>>* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  repo_->searchUsers(keyword, excludeUserId, limit, out);
}

} // namespace im