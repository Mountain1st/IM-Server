#include "test_util.hpp"

#include "token_store.hpp"
#include "user_repository.hpp"
#include "user_store.hpp"

#include <memory>
#include <string>

using namespace im;

// 全失败的假仓库：fetchUser 恒返回 1（账号不存在），每次登录失败都记限流
class AlwaysFailRepo : public UserRepository {
 public:
  int insertUser(const std::string&, const std::string&, const std::string&,
                 const std::string&, int64_t*) override {
    return 500;
  }
  int fetchUser(const std::string&, int64_t*, std::string*, std::string*,
                std::string*) override {
    return 1;
  }
  int fetchUserInfo(const std::string&, int64_t*, std::string*) override { return 1; }
  bool userExists(int64_t) override { return false; }
  bool fetchUserNameById(int64_t, std::string*) override { return false; }
  bool fetchAccountById(int64_t, std::string*) override { return false; }
  int fetchSettings(int64_t, int*, int*, int*) override { return 1; }
  int updateSettings(int64_t, int, int, int) override { return 1; }
  void searchUsers(const std::string&, int64_t, int,
                   std::vector<std::pair<std::string, std::string>>*) override {}
};

static int tryLogin(UserStore& s, const std::string& account) {
  int64_t uid = 0;
  std::string uname, token;
  return s.login(account, "wrongpw", &uid, &uname, &token);
}

TEST(locked_victim_survives_table_wash) {
  auto store = std::make_shared<UserStore>(std::make_shared<AlwaysFailRepo>(),
                                           std::make_shared<MemoryTokenStore>());
  const std::string victim = "10000000001";
  // 受害者失败 5 次，第 6 次应被锁定（429）
  for (int i = 0; i < 5; ++i) CHECK_EQ(tryLogin(*store, victim), 403);
  CHECK_EQ(tryLogin(*store, victim), 429);
  // 攻击者用 1100 个假账号各失败 1 次（超 1024 表上限，触发淘汰洗表）
  for (int i = 0; i < 1100; ++i) {
    CHECK_EQ(tryLogin(*store, "2000000" + std::to_string(1000 + i)), 403);
  }
  // 洗表后受害者仍应被锁定（429），计数不能被清零
  CHECK_EQ(tryLogin(*store, victim), 429);
}

int main() {
  RUN(locked_victim_survives_table_wash);
  std::printf("login_guard_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
