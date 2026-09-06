#include "test_util.hpp"

#include "app_util.hpp"
#include "token_store.hpp"
#include "user_store.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace im;

// 内存 fake：不碰 MySQL，供 resolveTarget 单测。
class FakeUserRepo : public UserRepository {
 public:
  std::unordered_map<std::string, int64_t> byAccount;  // account -> uid
  std::unordered_set<int64_t> ids;
  bool failNext = false;  // 置 true 则下一次 fetchUserInfo 返回 2（模拟 DB 错误）

  int insertUser(const std::string&, const std::string&, const std::string&,
                 const std::string&, int64_t*) override {
    return 500;
  }
  int fetchUser(const std::string&, int64_t*, std::string*, std::string*,
                std::string*) override {
    return 1;
  }
  int fetchUserInfo(const std::string& account, int64_t* userId,
                    std::string* username) override {
    if (failNext) {
      failNext = false;
      return 2;
    }
    auto it = byAccount.find(account);
    if (it == byAccount.end()) return 1;
    *userId = it->second;
    if (username) *username = "nick";
    return 0;
  }
  bool userExists(int64_t userId) override { return ids.count(userId) != 0; }
  bool fetchUserNameById(int64_t, std::string*) override { return false; }
  bool fetchAccountById(int64_t, std::string*) override { return false; }
  int fetchSettings(int64_t, int*, int*, int*) override { return 1; }
  int updateSettings(int64_t, int, int, int) override { return 0; }
  void searchUsers(const std::string&, int64_t, int,
                   std::vector<std::pair<std::string, std::string>>*) override {}
};

static std::unique_ptr<UserStore> makeStore(FakeUserRepo** out = nullptr) {
  auto repo = std::make_shared<FakeUserRepo>();
  if (out) *out = repo.get();
  repo->byAccount["13800138000"] = 7;
  repo->ids.insert(7);
  repo->ids.insert(121);
  return std::make_unique<UserStore>(repo, std::make_shared<MemoryTokenStore>());
}

TEST(target_account_string) {
  auto s = makeStore();
  json d = json{{"to", "13800138000"}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), 7);
}

TEST(target_account_number) {
  auto s = makeStore();
  json d = json{{"to", 13800138000LL}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), 7);
}

TEST(target_userid_compat) {
  auto s = makeStore();
  json d = json{{"to", 121}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), 121);
}

TEST(target_userid_missing) {
  auto s = makeStore();
  json d = json{{"to", 999999}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), -2);
}

TEST(target_account_missing) {
  auto s = makeStore();
  json d = json{{"to", "13900139000"}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), -2);
}

TEST(target_zero_rejected) {
  auto s = makeStore();
  json d = json{{"to", 0}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), -1);
}

TEST(target_garbage_rejected) {
  auto s = makeStore();
  CHECK_EQ(resolveTarget(s.get(), json{{"to", "abc"}}, "to"), -1);
  CHECK_EQ(resolveTarget(s.get(), json{{"to", ""}}, "to"), -1);
  CHECK_EQ(resolveTarget(s.get(), json::object(), "to"), -1);
}

TEST(target_dberror) {
  FakeUserRepo* repo = nullptr;
  auto s = makeStore(&repo);
  repo->failNext = true;
  json d = json{{"to", "13800138000"}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), -3);
}

TEST(valid_account_rules) {
  CHECK(validAccount("13800138000"));
  CHECK(!validAccount("123"));
  CHECK(!validAccount("abc"));
  CHECK(!validAccount("138001380001"));  // 12 位超长
}

int main() {
  RUN(target_account_string);
  RUN(target_account_number);
  RUN(target_userid_compat);
  RUN(target_userid_missing);
  RUN(target_account_missing);
  RUN(target_zero_rejected);
  RUN(target_garbage_rejected);
  RUN(target_dberror);
  RUN(valid_account_rules);
  std::printf("target_parse_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
