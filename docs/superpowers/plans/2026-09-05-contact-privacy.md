# 联系人功能 + 隐私设置 + 临时对话 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 在 IM 服务器（协议 v1.7）上实现联系人体系：搜索用户、添加/同意/拒绝/删除联系人、三个隐私开关（临时对话/可搜索/可添加）、自己对话框消息落库，并将 `msg.send` 按联系人+临时对话规则校验。

**架构：** 新增独立 `ContactRepository`（MySQL 持久化 contacts/friend_requests/用户设置），仿现有 `MessageRepository`/`UserRepository` 分层模式；`AppServer` 注入 `contactRepo_` 并新增 9 个指令处理函数与 3 个 S2C 推送事件；`msg.send`/`chat.history`/`msg.read` 适配新权限规则；数据库 schema 增表加字段。

**技术栈：** C++17、epoll + 线程池、自实现 WebSocket、MySQL（libmysqlclient）、可选 Redis（hiredis）、CMake。

**规格：** `docs/superpowers/specs/2026-09-05-contact-privacy-design.md`

**测试基线：** `bash scripts/test_biz.sh`（内存模式 72/72）、`IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380 bash scripts/test_biz.sh`（Redis 模式 72/72）、`bash scripts/test_origin.sh`、`cd build && ctest`

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `db/schema.sql` | 新增 contacts / friend_requests 表 + users 三字段（修改） |
| `src/contact_repository.hpp` | ContactRepository 接口：联系人/请求/设置（新增） |
| `src/contact_repository.cpp` | ContactRepository MySQL 实现 + isContact 内存缓存（新增） |
| `src/user_repository.hpp` | UserRepository 接口加 fetchSettings/updateSettings/searchUsers（修改） |
| `src/mysql_user_repository.hpp` | 声明 settings/search 实现（修改） |
| `src/mysql_user_repository.cpp` | 实现 settings 查询/更新、searchUsers（修改） |
| `src/appserver.hpp` | 新增 9 个处理函数声明 + contactRepo_ 成员（修改） |
| `src/appserver.cpp` | 路由、权限校验、自己对话框落库、推送事件、离线补发（修改） |
| `CMakeLists.txt` | 加 contact_repository.cpp（修改） |
| `scripts/biz_test.py` | 新用例 + 更新 to:0 用例（修改） |
| `docs/protocol.md` `docs/client_api.md` `docs/server-architecture-learning-guide.md` + docs/docs/ | v1.8 文档（修改） |

---

## 任务 1：数据库 schema 变更

**文件：** 修改 `db/schema.sql`

- [ ] **步骤 1：在 schema.sql 末尾追加**

```sql
-- 联系人关系表（双向各一条，删除时删两条）
CREATE TABLE IF NOT EXISTS contacts (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id    BIGINT UNSIGNED NOT NULL,
  contact_id BIGINT UNSIGNED NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_pair (user_id, contact_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 好友请求表：0=pending 1=accepted 2=rejected
CREATE TABLE IF NOT EXISTS friend_requests (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  from_user  BIGINT UNSIGNED NOT NULL,
  to_user    BIGINT UNSIGNED NOT NULL,
  status     TINYINT NOT NULL DEFAULT 0,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_from_to (from_user, to_user)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

- [ ] **步骤 2：在现有 MySQL 执行迁移（新增字段 + 新表）**

运行：
```bash
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db -e "
ALTER TABLE users ADD COLUMN allow_temp_chat TINYINT NOT NULL DEFAULT 1;
ALTER TABLE users ADD COLUMN searchable TINYINT NOT NULL DEFAULT 1;
ALTER TABLE users ADD COLUMN allow_add TINYINT NOT NULL DEFAULT 1;
CREATE TABLE IF NOT EXISTS contacts (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  contact_id BIGINT UNSIGNED NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_pair (user_id, contact_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE TABLE IF NOT EXISTS friend_requests (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  from_user BIGINT UNSIGNED NOT NULL,
  to_user BIGINT UNSIGNED NOT NULL,
  status TINYINT NOT NULL DEFAULT 0,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_from_to (from_user, to_user)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;"
```
预期：无错误输出；再次运行不报错（IF NOT EXISTS / 字段已存在则 ALTER 失败属预期，只需执行一次）。

- [ ] **步骤 3：验证**

运行：`MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -e "USE IMS_reactor_db; SHOW COLUMNS FROM users; SHOW TABLES;" 2>/dev/null | grep -E 'allow_temp_chat|searchable|allow_add|contacts|friend_requests'`
预期：三字段 + 两表均出现。

- [ ] **步骤 4：Commit**

```bash
git add db/schema.sql
git commit -m "feat: 联系人/好友请求表 + 用户三隐私开关字段（schema）"
```

---

## 任务 2：ContactRepository 接口与实现

**文件：**
- 创建：`src/contact_repository.hpp`
- 创建：`src/contact_repository.cpp`

- [ ] **步骤 1：编写 contact_repository.hpp**

```cpp
#pragma once

#include "mysql_client.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace im {

// 联系人/好友请求/用户隐私设置的 MySQL 存储。
// 仿 MySqlMessageRepository：单连接 + 内部互斥锁串行化 DB 访问。
// isContact 用内存缓存（contacts_ 集合）加速 msg.send 热路径，
// 启动全量加载；成对/删除时同步刷新缓存。
class ContactRepository {
public:
  ContactRepository(const std::string& host, unsigned port, const std::string& user,
                    const std::string& password, const std::string& db);

  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  // ---- 联系人 ----
  bool isContact(int64_t a, int64_t b);                      // a 与 b 是否为联系人（走缓存）
  bool addContactPair(int64_t a, int64_t b);                 // 双向插入（幂等：已存在不报错）
  bool deleteContactPair(int64_t a, int64_t b);              // 双向删除
  std::vector<int64_t> listContactIds(int64_t userId);       // 我的联系人 userId 列表

  // ---- 好友请求 ----
  // 返回：0=成功新建；1=已存在 pending 或已接受（重复请求）；2=数据库错误。
  int addRequest(int64_t from, int64_t to);
  // 返回：0=成功接受（标记 accepted + 建立联系人关系）；1=无 pending 请求；2=数据库错误。
  int acceptRequest(int64_t to, int64_t from);
  // 返回：0=成功拒绝；1=无 pending 请求；2=数据库错误。
  int rejectRequest(int64_t to, int64_t from);
  // 收到的 pending 请求列表（from_user, created_at 秒）
  struct PendingRequest {
    int64_t fromUser;
    int64_t ts;
  };
  std::vector<PendingRequest> listPendingRequests(int64_t to);

private:
  void loadContactsCache();

  MySqlClient client_;
  std::mutex mutex_; // 串行化 DB 访问 + 缓存读写
  // 已加载的 (a,b) 有向关系缓存；isContact(a,b) 查 (a,b)
  std::unordered_set<std::string> contacts_; // 键格式 "a|b"
};

} // namespace im
```

- [ ] **步骤 2：编写 contact_repository.cpp**

```cpp
#include "contact_repository.hpp"

#include <ctime>
#include <iostream>

namespace im {

ContactRepository::ContactRepository(const std::string& host, unsigned port,
                                     const std::string& user, const std::string& password,
                                     const std::string& db)
  : client_(host, port, user, password, db) {
  loadContactsCache();
}

void ContactRepository::loadContactsCache() {
  if (!client_.ok()) return;
  MYSQL* c = client_.handle();
  if (mysql_query(c, "SELECT user_id, contact_id FROM contacts")) {
    std::cerr << "[mysql] 加载联系人缓存失败: " << client_.errStr() << std::endl;
    return;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0] && row[1]) {
      contacts_.emplace(std::string(row[0]) + "|" + row[1]);
    }
  }
  mysql_free_result(res);
  std::cout << "[mysql] 已加载 " << contacts_.size() << " 条联系人关系到缓存" << std::endl;
}

bool ContactRepository::isContact(int64_t a, int64_t b) {
  std::lock_guard<std::mutex> lock(mutex_);
  return contacts_.count(std::to_string(a) + "|" + std::to_string(b)) != 0;
}

bool ContactRepository::addContactPair(int64_t a, int64_t b) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  const std::string sql =
      "INSERT IGNORE INTO contacts (user_id, contact_id) VALUES (?, ?), (?, ?)";
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(), sql,
      {std::to_string(a), std::to_string(b), std::to_string(b), std::to_string(a)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 添加联系人失败 errno=" << errNo << std::endl;
    return false;
  }
  mysql_stmt_close(stmt);
  contacts_.emplace(std::to_string(a) + "|" + std::to_string(b));
  contacts_.emplace(std::to_string(b) + "|" + std::to_string(a));
  return true;
}

bool ContactRepository::deleteContactPair(int64_t a, int64_t b) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "DELETE FROM contacts WHERE (user_id = ? AND contact_id = ?) "
      "OR (user_id = ? AND contact_id = ?)",
      {std::to_string(a), std::to_string(b), std::to_string(b), std::to_string(a)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 删除联系人失败 errno=" << errNo << std::endl;
    return false;
  }
  mysql_stmt_close(stmt);
  contacts_.erase(std::to_string(a) + "|" + std::to_string(b));
  contacts_.erase(std::to_string(b) + "|" + std::to_string(a));
  return true;
}

std::vector<int64_t> ContactRepository::listContactIds(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int64_t> out;
  if (!client_.ok()) return out;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "SELECT contact_id FROM contacts WHERE user_id = ?", {std::to_string(userId)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 查询联系人列表失败 errno=" << errNo << std::endl;
    return out;
  }
  int64_t cid = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &cid;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    std::cerr << "[mysql] 联系人列表结果绑定失败: " << client_.errStr() << std::endl;
    mysql_stmt_close(stmt);
    return out;
  }
  while (mysql_stmt_fetch(stmt) == 0) out.push_back(cid);
  mysql_stmt_close(stmt);
  return out;
}

int ContactRepository::addRequest(int64_t from, int64_t to) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "INSERT INTO friend_requests (from_user, to_user, status) VALUES (?, ?, 0)",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    if (errNo == 1062) return 1; // 唯一键冲突：已有请求
    std::cerr << "[mysql] 添加好友请求失败 errno=" << errNo << std::endl;
    return 2;
  }
  mysql_stmt_close(stmt);
  return 0;
}

int ContactRepository::acceptRequest(int64_t to, int64_t from) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "UPDATE friend_requests SET status = 1 WHERE from_user = ? AND to_user = ? AND status = 0",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 接受好友请求失败 errno=" << errNo << std::endl;
    return 2;
  }
  const int affected = static_cast<int>(mysql_stmt_affected_rows(stmt));
  mysql_stmt_close(stmt);
  if (affected <= 0) return 1; // 无 pending 请求
  // 建立联系人关系（双向）
  MYSQL_STMT* s2 = mysqlStmtExec(client_.handle(),
      "INSERT IGNORE INTO contacts (user_id, contact_id) VALUES (?, ?), (?, ?)",
      {std::to_string(to), std::to_string(from), std::to_string(from), std::to_string(to)},
      &errNo);
  if (!s2) {
    std::cerr << "[mysql] 接受请求后建联系人失败 errno=" << errNo << std::endl;
    return 2;
  }
  mysql_stmt_close(s2);
  contacts_.emplace(std::to_string(to) + "|" + std::to_string(from));
  contacts_.emplace(std::to_string(from) + "|" + std::to_string(to));
  return 0;
}

int ContactRepository::rejectRequest(int64_t to, int64_t from) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 2;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "UPDATE friend_requests SET status = 2 WHERE from_user = ? AND to_user = ? AND status = 0",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 拒绝好友请求失败 errno=" << errNo << std::endl;
    return 2;
  }
  const int affected = static_cast<int>(mysql_stmt_affected_rows(stmt));
  mysql_stmt_close(stmt);
  return affected > 0 ? 0 : 1;
}

std::vector<ContactRepository::PendingRequest> ContactRepository::listPendingRequests(int64_t to) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PendingRequest> out;
  if (!client_.ok()) return out;
  const std::string sql =
      "SELECT from_user, created_at FROM friend_requests WHERE to_user = " +
      std::to_string(to) + " AND status = 0 ORDER BY id ASC";
  if (mysql_query(client_.handle(), sql.c_str())) {
    std::cerr << "[mysql] 查询好友请求失败: " << client_.errStr() << std::endl;
    return out;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return out;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    PendingRequest pr;
    if (row[0]) pr.fromUser = std::strtoll(row[0], nullptr, 10);
    pr.ts = (row[1] && row[1][0]) ? static_cast<int64_t>(std::strtoll(row[1], nullptr, 10))
                                  : static_cast<int64_t>(std::time(nullptr));
    out.push_back(pr);
  }
  mysql_free_result(res);
  return out;
}

} // namespace im
```

注意：`listPendingRequests` 中 `created_at` 是 TIMESTAMP（字符串如 `2026-09-05 12:00:00`），`strtoll` 会解析为 0 → 回退当前时间。可接受；若需真实 ts 应 `UNIX_TIMESTAMP(created_at)`，见步骤 3 优化。

- [ ] **步骤 3：优化 created_at 为 Unix 秒**

将 `listPendingRequests` 的 SQL 改为：
```cpp
  const std::string sql =
      "SELECT from_user, UNIX_TIMESTAMP(created_at) FROM friend_requests WHERE to_user = " +
      std::to_string(to) + " AND status = 0 ORDER BY id ASC";
```
此时 row[1] 为十进制秒字符串，`strtoll` 正确解析。删除步骤 2 中 `row[1][0] ? ... : time()` 的 fallback 逻辑，改为 `pr.ts = row[1] ? std::strtoll(row[1], nullptr, 10) : 0;`。

- [ ] **步骤 4：编译验证**

运行：`g++ -std=c++17 -Isrc -Ithird_party -c src/contact_repository.cpp -o /tmp/cr.o $(pkg-config --cflags mysqlclient) 2>&1 | head`
预期：无输出（成功编译）。注：临时用 g++ 单独编译验证语法，正式集成见任务 5。

- [ ] **步骤 5：Commit**

```bash
git add src/contact_repository.hpp src/contact_repository.cpp
git commit -m "feat: ContactRepository 联系人/好友请求/设置 MySQL 存储（含 isContact 缓存）"
```

---

## 任务 3：UserRepository 设置与搜索

**文件：**
- 修改：`src/user_repository.hpp`
- 修改：`src/mysql_user_repository.hpp`
- 修改：`src/mysql_user_repository.cpp`

- [ ] **步骤 1：user_repository.hpp 增加接口**

在 `fetchAccountById` 声明后追加：
```cpp
  // 用户隐私设置（users 表 allow_temp_chat/searchable/allow_add）。
  // 返回：0=找到并写出三个开关；1=用户不存在；2=数据库错误。
  virtual int fetchSettings(int64_t userId, int* allowTempChat, int* searchable,
                            int* allowAdd) = 0;
  // 更新用户隐私设置（任一字段 < 0 表示不修改）。
  virtual void updateSettings(int64_t userId, int allowTempChat, int searchable,
                              int allowAdd) = 0;
  // 模糊搜索用户（account 或 username 匹配 keyword），排除 excludeUserId。
  // 仅返回 searchable=1 的用户。结果追加到 out（each: account, username）。
  virtual void searchUsers(const std::string& keyword, int64_t excludeUserId,
                           int limit,
                           std::vector<std::pair<std::string, std::string>>* out) = 0;
```

- [ ] **步骤 2：mysql_user_repository.hpp 声明**

在类中追加 public 覆盖声明：
```cpp
  int fetchSettings(int64_t userId, int* allowTempChat, int* searchable,
                    int* allowAdd) override;
  void updateSettings(int64_t userId, int allowTempChat, int searchable,
                      int allowAdd) override;
  void searchUsers(const std::string& keyword, int64_t excludeUserId, int limit,
                   std::vector<std::pair<std::string, std::string>>* out) override;
```

- [ ] **步骤 3：mysql_user_repository.cpp 实现**

在文件末尾 `fetchAccountById` 后追加：
```cpp
int MySqlUserRepository::fetchSettings(int64_t userId, int* allowTempChat, int* searchable,
                                       int* allowAdd) {
  if (!client_.ok()) return 2;
  MYSQL* c = client_.handle();
  const std::string sql = "SELECT allow_temp_chat, searchable, allow_add FROM users WHERE id = ? LIMIT 1";
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(c, sql, {std::to_string(userId)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 查询设置失败 errno=" << errNo << std::endl;
    return 2;
  }
  int v0 = 0, v1 = 0, v2 = 0;
  MYSQL_BIND res[3];
  memset(res, 0, sizeof res);
  for (int i = 0; i < 3; ++i) {
    res[i].buffer_type = MYSQL_TYPE_LONG;
    res[i].buffer = (i == 0) ? &v0 : (i == 1) ? &v1 : &v2;
  }
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    std::cerr << "[mysql] 设置结果绑定失败: " << client_.errStr() << std::endl;
    mysql_stmt_close(stmt);
    return 2;
  }
  const int rc = mysql_stmt_fetch(stmt);
  mysql_stmt_close(stmt);
  if (rc != 0) return 1; // 用户不存在
  if (allowTempChat) *allowTempChat = v0;
  if (searchable) *searchable = v1;
  if (allowAdd) *allowAdd = v2;
  return 0;
}

void MySqlUserRepository::updateSettings(int64_t userId, int allowTempChat, int searchable,
                                         int allowAdd) {
  if (!client_.ok()) return;
  // 任一 < 0 表示不修改该字段（用 COALESCE 简化）
  const std::string sql =
      "UPDATE users SET allow_temp_chat = CASE WHEN ? < 0 THEN allow_temp_chat ELSE ? END, "
      "searchable = CASE WHEN ? < 0 THEN searchable ELSE ? END, "
      "allow_add = CASE WHEN ? < 0 THEN allow_add ELSE ? END WHERE id = ?";
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(), sql,
      {std::to_string(allowTempChat), std::to_string(allowTempChat),
       std::to_string(searchable), std::to_string(searchable),
       std::to_string(allowAdd), std::to_string(allowAdd),
       std::to_string(userId)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 更新设置失败 errno=" << errNo << std::endl;
    return;
  }
  mysql_stmt_close(stmt);
}

void MySqlUserRepository::searchUsers(const std::string& keyword, int64_t excludeUserId,
                                      int limit,
                                      std::vector<std::pair<std::string, std::string>>* out) {
  if (!client_.ok() || !out) return;
  if (limit <= 0) limit = 20;
  if (limit > 50) limit = 50;
  const std::string like = "%" + keyword + "%";
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "SELECT account, username FROM users WHERE searchable = 1 AND id != ? "
      "AND (account LIKE ? OR username LIKE ?) ORDER BY id ASC LIMIT ?",
      {std::to_string(excludeUserId), like, like, std::to_string(limit)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 搜索用户失败 errno=" << errNo << std::endl;
    return;
  }
  char acctBuf[128] = {0}, nameBuf[128] = {0};
  unsigned long acctLen = 0, nameLen = 0;
  bool acctNull = false, nameNull = false;
  MYSQL_BIND res[2];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_STRING;
  res[0].buffer = acctBuf;
  res[0].buffer_length = sizeof acctBuf;
  res[0].length = &acctLen;
  res[0].is_null = &acctNull;
  res[1].buffer_type = MYSQL_TYPE_STRING;
  res[1].buffer = nameBuf;
  res[1].buffer_length = sizeof nameBuf;
  res[1].length = &nameLen;
  res[1].is_null = &nameNull;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    std::cerr << "[mysql] 搜索结果绑定失败: " << client_.errStr() << std::endl;
    mysql_stmt_close(stmt);
    return;
  }
  while (mysql_stmt_fetch(stmt) == 0) {
    out->emplace_back(acctNull ? std::string() : std::string(acctBuf, acctLen),
                      nameNull ? std::string() : std::string(nameBuf, nameLen));
  }
  mysql_stmt_close(stmt);
}
```

- [ ] **步骤 4：UserStore 暴露 settings/search（如需经 UserStore 访问）**

说明：`AppServer` 直接持有 `contactRepo_`，但 settings/search 挂在 `userStore_`（其内部持 `repo_`）。**决定：不经 UserStore，AppServer 直接持 `std::unique_ptr<MySqlUserRepository>` 不现实（userStore_ 已持 repo_ 且带锁）。** 改为在 `UserStore` 增加转发方法：

在 `src/user_store.hpp` 的 public 区追加：
```cpp
  // 隐私设置转发（见 UserRepository）
  int fetchSettings(int64_t userId, int* allowTempChat, int* searchable, int* allowAdd);
  void updateSettings(int64_t userId, int allowTempChat, int searchable, int allowAdd);
  void searchUsers(const std::string& keyword, int64_t excludeUserId, int limit,
                   std::vector<std::pair<std::string, std::string>>* out);
```

在 `src/user_store.cpp` 末尾追加：
```cpp
int UserStore::fetchSettings(int64_t userId, int* allowTempChat, int* searchable, int* allowAdd) {
  std::lock_guard<std::mutex> lock(mutex_);
  return repo_->fetchSettings(userId, allowTempChat, searchable, allowAdd);
}

void UserStore::updateSettings(int64_t userId, int allowTempChat, int searchable, int allowAdd) {
  std::lock_guard<std::mutex> lock(mutex_);
  repo_->updateSettings(userId, allowTempChat, searchable, allowAdd);
}

void UserStore::searchUsers(const std::string& keyword, int64_t excludeUserId, int limit,
                            std::vector<std::pair<std::string, std::string>>* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  repo_->searchUsers(keyword, excludeUserId, limit, out);
}
```

- [ ] **步骤 5：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error' | head`（此时 AppServer 未引用新接口，纯接口扩展应编译通过）
预期：无 error。

- [ ] **步骤 6：Commit**

```bash
git add src/user_repository.hpp src/mysql_user_repository.hpp src/mysql_user_repository.cpp src/user_store.hpp src/user_store.cpp
git commit -m "feat: UserRepository 隐私设置查询/更新 + 用户模糊搜索（searchable 过滤）"
```

---

## 任务 4：resolveTarget 去掉 to==0 自己语义

**文件：** 修改 `src/appserver.cpp:77-115`

- [ ] **步骤 1：修改 resolveTarget 注释与返回约定**

`"0"/空` 从「自己对话框」改为「参数错误」：将第 93 行 `if (s.empty() || s == "0") return 0;` 改为 `if (s.empty()) return -1;`，并更新函数头注释：
```cpp
// 解析消息收发目标字段。协议 v1.8：字段为登录账号 account；
// 兼容旧协议传数字 userId。自己对话框通过 to==自己 account 表达，
// 不再有 to:0 特例。
// 返回值：>0=目标 userId；-1=参数错误（400）；-2=目标不存在（404）；-3=存储异常（500）。
```

- [ ] **步骤 2：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error' | head`
预期：可能因 `to==0` 比较无错（0 仍可能是合法返回？不，现在永不返回 0，但调用方比较无害）。确认无 error。

- [ ] **步骤 3：Commit**

```bash
git add src/appserver.cpp
git commit -m "refactor: resolveTarget 移除 to:0 自己对话框特例（自己对话框用 to==自己 account）"
```

---

## 任务 5：AppServer 集成 —— 成员、路由、权限校验、自己对话框落库

**文件：**
- 修改：`src/appserver.hpp`
- 修改：`src/appserver.cpp`

- [ ] **步骤 1：appserver.hpp 增加声明与成员**

在 `#include "msg_dedup.hpp"` 前加 `#include "contact_repository.hpp"`。
在 private 方法区（`onRead` 之后）追加：
```cpp
  void onSearch(const std::shared_ptr<Session>& session, const json& data);
  void onContactRequest(const std::shared_ptr<Session>& session, const json& data);
  void onContactAccept(const std::shared_ptr<Session>& session, const json& data);
  void onContactReject(const std::shared_ptr<Session>& session, const json& data);
  void onContactList(const std::shared_ptr<Session>& session, const json& data);
  void onContactRequests(const std::shared_ptr<Session>& session, const json& data);
  void onContactDelete(const std::shared_ptr<Session>& session, const json& data);
  void onSettingsGet(const std::shared_ptr<Session>& session, const json& data);
  void onSettingsSet(const std::shared_ptr<Session>& session, const json& data);
  // 推送 pending 好友请求（登录/重连后补发）
  void pushPendingRequests(int64_t userId, const std::shared_ptr<Session>& session);
  // 发送权限校验辅助：自己/联系人/临时对话
  bool canSendTo(int64_t from, int64_t to, int* allowTempChat);
```
在 `msgRepo_` 后追加成员：
```cpp
  std::unique_ptr<ContactRepository> contactRepo_; // 联系人/请求/设置存储
```

- [ ] **步骤 2：构造函数初始化 contactRepo_**

在 `AppServer` 委托构造的初始化列表（`msgRepo_` 附近）追加：
```cpp
      contactRepo_(std::make_unique<ContactRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
```
并在 `msgRepo_->ok()` 校验后加：
```cpp
  if (!contactRepo_->ok()) {
    throw std::runtime_error("联系人存储连接失败");
  }
  std::cout << "[db] 联系人存储: MySQL (host=" << userConfig.mysqlHost << ":"
            << userConfig.mysqlPort << ", db=" << userConfig.mysqlDb << ")" << std::endl;
```

- [ ] **步骤 3：onEnvelope 路由**

在 `onEnvelope` 的 `msg.read` 分支后追加：
```cpp
    if (type == "user.search") {
      onSearch(session, data);
      return;
    }
    if (type == "contact.request") {
      onContactRequest(session, data);
      return;
    }
    if (type == "contact.accept") {
      onContactAccept(session, data);
      return;
    }
    if (type == "contact.reject") {
      onContactReject(session, data);
      return;
    }
    if (type == "contact.list") {
      onContactList(session, data);
      return;
    }
    if (type == "contact.requests") {
      onContactRequests(session, data);
      return;
    }
    if (type == "contact.delete") {
      onContactDelete(session, data);
      return;
    }
    if (type == "settings.get") {
      onSettingsGet(session, data);
      return;
    }
    if (type == "settings.set") {
      onSettingsSet(session, data);
      return;
    }
```

- [ ] **步骤 4：修改 onSend —— 权限校验 + 自己对话框落库**

将当前 `onSend` 中权限部分替换为（保留 chatType/content/大小/幂等/限流校验不变）：

在 resolveTarget 错误处理后、幂等检查前，插入权限校验（`to` 解析后目标已确定）：
```cpp
  // 权限校验：自己对话框 / 联系人 / 目标开启临时对话
  if (to != session->userId()) {
    int allowTemp = 0;
    const bool isContact = contactRepo_->isContact(session->userId(), to);
    if (!isContact) {
      const int frc = userStore_->fetchSettings(to, &allowTemp, nullptr, nullptr);
      if (frc != 0) { // 目标不存在或 DB 错误（resolveTarget 已查过，此处兜底）
        ack(500, "服务器内部错误");
        return;
      }
      if (allowTemp != 1) {
        ack(403, "对方未开启临时对话，请先添加联系人");
        return;
      }
    }
  }
```

替换"自己对话框"分支（原 `to == 0 || to == session->userId()`）为：
```cpp
  // 自己对话框（to==自己账号）：落库持久化，只回 ack 不回显 msg.recv（发送方即自己）
  if (to == session->userId()) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::string messageId = nextMessageId();
    if (!msgRepo_->saveMessage(messageId, session->userId(), to, content, now,
                               clientMsgId, chatType, kMsgDelivered)) {
      ack(500, "服务器内部错误");
      return;
    }
    msgDedup_->remember(session->userId(), clientMsgId);
    ack(0, "ok");
    return;
  }
```
其余非自己对话框逻辑（targetOnline / saveMessage / recv 推送）保持不变。

- [ ] **步骤 5：修改 onHistory —— 自己对话框返回真实历史**

将原 `if (to == 0 || to == session->userId()) { 空列表 return }` 分支删除；将 `int limit` 之后的 `fetchHistory(session->userId(), to, limit)` 保留——当 `to == session->userId()` 时 SQL 自然查询自己发给自己的消息（`(from=A AND to=A) OR (from=A AND to=A)` 等价）。即删除整个"自己对话框空历史"特判块。

- [ ] **步骤 6：修改 onRead —— 自己对话框直接成功**

将原 `if (from == 0 || from == session->userId()) { return ok }` 保留（语义一致，无对端消息直接成功）。仅当 from==自己时直接 ok；from 为其它用户走 markRead。当前代码已正确，无需改动（只需确认 `from == 0` 分支已由 resolveTarget 改为 -1，不会触发）。

- [ ] **步骤 7：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error' | head`
预期：无 error（onSearch 等尚未实现会报链接错误——**本任务需与任务 6 一起完成再编译**，见任务 6 步骤 5）。

- [ ] **步骤 8：Commit（与任务 6 合并）**

---

## 任务 6：AppServer 新增指令处理函数 + 推送 + 离线补发

**文件：** 修改 `src/appserver.cpp`

- [ ] **步骤 1：实现 onSearch**

在 `onRead` 后追加：
```cpp
void AppServer::onSearch(const std::shared_ptr<Session>& session, const json& data) {
  std::string keyword = data.value("keyword", "");
  if (keyword.empty()) {
    session->reply(makeResult("user.search_res", 400, "参数错误", json::object()));
    return;
  }
  int limit = data.value("limit", 20);
  std::vector<std::pair<std::string, std::string>> users;
  userStore_->searchUsers(keyword, session->userId(), limit, &users);
  json arr = json::array();
  for (const auto& u : users) {
    int64_t uid = 0;
    std::string nick;
    userStore_->lookupAccount(u.first, &uid, &nick);
    arr.push_back(json{{"account", u.first}, {"username", u.second},
                       {"isContact", contactRepo_->isContact(session->userId(), uid)}});
  }
  session->reply(makeResult("user.search_res", 0, "ok", json{{"users", arr}}));
}
```

- [ ] **步骤 2：实现 contact.request/accept/reject**

```cpp
void AppServer::onContactRequest(const std::shared_ptr<Session>& session, const json& data) {
  const std::string account = data.value("account", "");
  const int64_t to = resolveTarget(userStore_.get(), data, "account");
  if (to == -1) {
    session->reply(makeResult("contact.request_res", 400, "参数错误", json::object()));
    return;
  }
  if (to == -2) {
    session->reply(makeResult("contact.request_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (to == -3) {
    session->reply(makeResult("contact.request_res", 500, "服务器内部错误", json::object()));
    return;
  }
  const int64_t self = session->userId();
  if (to == self) {
    session->reply(makeResult("contact.request_res", 400, "不能添加自己为联系人", json::object()));
    return;
  }
  if (contactRepo_->isContact(self, to)) {
    session->reply(makeResult("contact.request_res", 409, "对方已是你的联系人", json::object()));
    return;
  }
  int allowAdd = 0;
  const int frc = userStore_->fetchSettings(to, nullptr, nullptr, &allowAdd);
  if (frc != 0) {
    session->reply(makeResult("contact.request_res", 500, "服务器内部错误", json::object()));
    return;
  }
  if (allowAdd != 1) {
    session->reply(makeResult("contact.request_res", 403, "对方未开启添加，请使用其他方式联系", json::object()));
    return;
  }
  const int rc = contactRepo_->addRequest(self, to);
  if (rc == 1) {
    session->reply(makeResult("contact.request_res", 409, "已存在待处理的请求", json::object()));
    return;
  }
  if (rc == 2) {
    session->reply(makeResult("contact.request_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("contact.request_res", 0, "ok", json::object()));
  // 推送给被发起人（在线）；离线由登录后补发（pushPendingRequests）
  std::string fromNick, fromAccount;
  userStore_->nickname(self, &fromNick);
  userStore_->accountOf(self, &fromAccount);
  json ev = {{"from", fromAccount}, {"fromNick", fromNick},
             {"ts", static_cast<int64_t>(std::time(nullptr))}};
  for (auto& t : onlineMgr_.sessions(to)) {
    t->reply(makeEnvelopeText("contact.request", ev));
  }
}

void AppServer::onContactAccept(const std::shared_ptr<Session>& session, const json& data) {
  const int64_t from = resolveTarget(userStore_.get(), data, "account");
  if (from == -1 || from == -2 || from == -3) {
    session->reply(makeResult("contact.accept_res", 400, "参数错误", json::object()));
    return;
  }
  const int rc = contactRepo_->acceptRequest(session->userId(), from);
  if (rc == 1) {
    session->reply(makeResult("contact.accept_res", 404, "不存在该待处理请求", json::object()));
    return;
  }
  if (rc == 2) {
    session->reply(makeResult("contact.accept_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("contact.accept_res", 0, "ok", json::object()));
  // 通知请求方：成为联系人
  std::string account, username;
  userStore_->accountOf(session->userId(), &account);
  userStore_->nickname(session->userId(), &username);
  json ev = {{"account", account}, {"username", username}};
  for (auto& t : onlineMgr_.sessions(from)) {
    t->reply(makeEnvelopeText("contact.added", ev));
  }
}

void AppServer::onContactReject(const std::shared_ptr<Session>& session, const json& data) {
  const int64_t from = resolveTarget(userStore_.get(), data, "account");
  if (from == -1 || from == -2 || from == -3) {
    session->reply(makeResult("contact.reject_res", 400, "参数错误", json::object()));
    return;
  }
  const int rc = contactRepo_->rejectRequest(session->userId(), from);
  if (rc == 1) {
    session->reply(makeResult("contact.reject_res", 404, "不存在该待处理请求", json::object()));
    return;
  }
  if (rc == 2) {
    session->reply(makeResult("contact.reject_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("contact.reject_res", 0, "ok", json::object()));
}
```

- [ ] **步骤 3：实现 contact.list / contact.requests / contact.delete**

```cpp
void AppServer::onContactList(const std::shared_ptr<Session>& session, const json& data) {
  const auto ids = contactRepo_->listContactIds(session->userId());
  json arr = json::array();
  for (const int64_t id : ids) {
    std::string account, username;
    userStore_->accountOf(id, &account);
    userStore_->nickname(id, &username);
    arr.push_back(json{{"account", account}, {"username", username}});
  }
  session->reply(makeResult("contact.list_res", 0, "ok", json{{"contacts", arr}}));
}

void AppServer::onContactRequests(const std::shared_ptr<Session>& session, const json& data) {
  const auto reqs = contactRepo_->listPendingRequests(session->userId());
  json arr = json::array();
  for (const auto& r : reqs) {
    std::string account, username;
    userStore_->accountOf(r.fromUser, &account);
    userStore_->nickname(r.fromUser, &username);
    arr.push_back(json{{"account", account}, {"username", username}, {"ts", r.ts}});
  }
  session->reply(makeResult("contact.requests_res", 0, "ok", json{{"requests", arr}}));
}

void AppServer::onContactDelete(const std::shared_ptr<Session>& session, const json& data) {
  const int64_t other = resolveTarget(userStore_.get(), data, "account");
  if (other == -1 || other == -2 || other == -3) {
    session->reply(makeResult("contact.delete_res", 400, "参数错误", json::object()));
    return;
  }
  if (other == session->userId()) {
    session->reply(makeResult("contact.delete_res", 400, "参数错误", json::object()));
    return;
  }
  if (!contactRepo_->isContact(session->userId(), other)) {
    session->reply(makeResult("contact.delete_res", 404, "对方不是你的联系人", json::object()));
    return;
  }
  contactRepo_->deleteContactPair(session->userId(), other);
  session->reply(makeResult("contact.delete_res", 0, "ok", json::object()));
  // 通知对方（在线）
  std::string account;
  userStore_->accountOf(session->userId(), &account);
  json ev = {{"account", account}};
  for (auto& t : onlineMgr_.sessions(other)) {
    t->reply(makeEnvelopeText("contact.deleted", ev));
  }
}
```

- [ ] **步骤 4：实现 settings.get / settings.set 与 pushPendingRequests**

```cpp
void AppServer::onSettingsGet(const std::shared_ptr<Session>& session, const json& data) {
  int allowTemp = 0, searchable = 0, allowAdd = 0;
  const int rc = userStore_->fetchSettings(session->userId(), &allowTemp, &searchable, &allowAdd);
  if (rc != 0) {
    session->reply(makeResult("settings.get_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("settings.get_res", 0, "ok",
      json{{"allowTempChat", allowTemp == 1},
           {"searchable", searchable == 1},
           {"allowAdd", allowAdd == 1}}));
}

void AppServer::onSettingsSet(const std::shared_ptr<Session>& session, const json& data) {
  const int allowTemp = data.contains("allowTempChat") ? (data["allowTempChat"].get<bool>() ? 1 : 0) : -1;
  const int searchable = data.contains("searchable") ? (data["searchable"].get<bool>() ? 1 : 0) : -1;
  const int allowAdd = data.contains("allowAdd") ? (data["allowAdd"].get<bool>() ? 1 : 0) : -1;
  userStore_->updateSettings(session->userId(), allowTemp, searchable, allowAdd);
  session->reply(makeResult("settings.set_res", 0, "ok", json::object()));
}

void AppServer::pushPendingRequests(int64_t userId,
                                    const std::shared_ptr<Session>& session) {
  const auto reqs = contactRepo_->listPendingRequests(userId);
  if (reqs.empty()) return;
  for (const auto& r : reqs) {
    std::string account, username;
    userStore_->accountOf(r.fromUser, &account);
    userStore_->nickname(r.fromUser, &username);
    json ev = {{"from", account}, {"fromNick", username}, {"ts", r.ts}};
    session->reply(makeEnvelopeText("contact.request", ev));
  }
}
```

- [ ] **步骤 5：登录/重连补发 pending 请求**

在 `onLogin`（`pushOfflineMessages(userId, session);` 后）与 `onToken`（同样位置）各追加一行：
```cpp
  pushPendingRequests(userId, session);
```

- [ ] **步骤 6：完整编译**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无 error/warning。

- [ ] **步骤 7：Commit（合并任务 5 步骤 8 一起）**

```bash
git add src/appserver.hpp src/appserver.cpp
git commit -m "feat: 联系人/搜索/设置指令与推送事件，msg.send 权限校验，自己对话框落库"
```

---

## 任务 7：CMake 集成

**文件：** 修改 `CMakeLists.txt`

- [ ] **步骤 1：加源文件**

在 `src/mysql_message_repository.cpp` 后追加 `src/contact_repository.cpp`：
```cmake
    src/mysql_message_repository.cpp
    src/contact_repository.cpp
```

- [ ] **步骤 2：完整构建**

运行：`cmake -S . -B build && cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无 error/warning。

- [ ] **步骤 3：Commit**

```bash
git add CMakeLists.txt
git commit -m "build: 集成 contact_repository.cpp"
```

---

## 任务 8：biz_test.py 更新与扩展

**文件：** 修改 `scripts/biz_test.py`

- [ ] **步骤 1：更新既有 to:0 用例**

将"自己对话框 to:0"相关用例改为 `to==自己 account`：
- `c1.send(env("msg.send", {"to": 0, "content": "x", "clientMsgId": "self0"}))` → `{"to": C, ...}`
- `chat.history to=0` 用例 → `to=C`（此时自己对话框**有历史**：自己发的消息）
- `msg.read from=0` → `from=C`

查找并替换 biz_test.py 中所有 `"to": 0`、`"to": 0`、`"from": 0` 为对应自己 account 变量。

- [ ] **步骤 2：新增"自己对话框落库 + 历史可查"用例**

在 self 测试段追加：
```python
    # v1.8：自己对话框落库，chat.history 可查到
    c1.send(env("chat.history", {"to": C}))
    r = json.loads(c1.recv())
    check("self history persisted", r["type"] == "chat.history_res"
          and r["data"]["code"] == 0
          and len(r["data"]["data"]["msgs"]) >= 3)  # self0/selfacct0/selfacct1...
```

- [ ] **步骤 3：新增联系人流程用例**

在文件末尾（登出段之前）追加完整联系人测试：
```python
    # ---- v1.8 联系人体系 ----
    # 准备独立账号：用 D/E/F 避免与前面账号干扰
    D = base + "8"; E = base + "9"; F = base + "10"
    d = WS(); register(d, D, "pw1234"); login(d, D, "pw1234")
    e = WS(); register(e, E, "pw1234"); login(e, E, "pw1234")
    f = WS(); register(f, F, "pw1234"); login(f, F, "pw1234")

    # 搜索：按账号关键字搜到对方
    d.send(env("user.search", {"keyword": E}))
    r = json.loads(d.recv())
    check("search by account", r["type"] == "user.search_res" and r["data"]["code"] == 0
          and any(u["account"] == E for u in r["data"]["data"]["users"]))
    # 搜索到自己不出现在结果
    d.send(env("user.search", {"keyword": D}))
    r = json.loads(d.recv())
    check("search excludes self", r["type"] == "user.search_res"
          and all(u["account"] != D for u in r["data"]["data"]["users"]))

    # 添加请求 → 对方收到 contact.request
    d.send(env("contact.request", {"account": E}))
    r = json.loads(d.recv())
    check("contact request ok", r["type"] == "contact.request_res" and r["data"]["code"] == 0)
    r = json.loads(e.recv())
    check("contact request pushed", r["type"] == "contact.request"
          and r["data"]["from"] == D)
    # 重复请求 → 409
    d.send(env("contact.request", {"account": E}))
    r = json.loads(d.recv())
    check("contact request dup 409", r["type"] == "contact.request_res"
          and r["data"]["code"] == 409)
    # 对方查看 pending 请求
    e.send(env("contact.requests", {}))
    r = json.loads(e.recv())
    check("contact requests list", r["type"] == "contact.requests_res"
          and r["data"]["code"] == 0
          and any(x["account"] == D for x in r["data"]["data"]["requests"]))
    # 同意 → 双方联系人；请求方收到 contact.added
    e.send(env("contact.accept", {"account": D}))
    r = json.loads(e.recv())
    check("contact accept ok", r["type"] == "contact.accept_res" and r["data"]["code"] == 0)
    r = json.loads(d.recv())
    check("contact added pushed", r["type"] == "contact.added" and r["data"]["account"] == E)
    # 联系人列表
    d.send(env("contact.list", {}))
    r = json.loads(d.recv())
    check("contact list has E", r["type"] == "contact.list_res"
          and any(x["account"] == E for x in r["data"]["data"]["contacts"]))
    # 成为联系人后互发消息成功
    r = send_msg(d, E, "hi contact", "ct1")
    check("contact send ok", r["data"]["code"] == 0)
    r = json.loads(e.recv())
    check("contact recv ok", r["type"] == "msg.recv" and r["data"]["from"] == D)

    # 拒绝流程：F 请求 D，D 拒绝
    f.send(env("contact.request", {"account": D}))
    json.loads(f.recv())
    json.loads(d.recv())  # contact.request 推送
    d.send(env("contact.reject", {"account": F}))
    r = json.loads(d.recv())
    check("contact reject ok", r["type"] == "contact.reject_res" and r["data"]["code"] == 0)
    f.send(env("contact.list", {}))
    r = json.loads(f.recv())
    check("rejected not in contacts", r["data"]["code"] == 0
          and all(x["account"] != D for x in r["data"]["data"]["contacts"]))

    # 删除联系人：D 删除 E，双向解除
    d.send(env("contact.delete", {"account": E}))
    r = json.loads(d.recv())
    check("contact delete ok", r["type"] == "contact.delete_res" and r["data"]["code"] == 0)
    e.send(env("contact.list", {}))
    r = json.loads(e.recv())
    check("contact deleted both sides", r["data"]["code"] == 0
          and all(x["account"] != D for x in r["data"]["data"]["contacts"]))

    d.close(); e.close(); f.close()
```

- [ ] **步骤 4：新增隐私开关用例**

在联系人测试后追加：
```python
    # ---- v1.8 隐私开关 ----
    g = WS(); register(g, base + "11", "pw1234"); login(g, base + "11", "pw1234")
    h = WS(); register(h, base + "12", "pw1234"); login(h, base + "12", "pw1234")
    # 默认三开关全开
    g.send(env("settings.get", {}))
    r = json.loads(g.recv())
    check("settings default on", r["type"] == "settings.get_res"
          and r["data"]["data"]["allowTempChat"] and r["data"]["data"]["searchable"]
          and r["data"]["data"]["allowAdd"])
    # 关临时对话：非联系人 h → g 发送 403
    g.send(env("settings.set", {"allowTempChat": False}))
    r = json.loads(g.recv())
    check("settings set temp off", r["type"] == "settings.set_res" and r["data"]["code"] == 0)
    r = send_msg(h, gacct, "temp off", "t1")  # gacct = base+"11"
    check("send to temp-off 403", r["type"] == "msg.ack" and r["data"]["code"] == 403)
    # 关搜索：搜不到
    g.send(env("settings.set", {"searchable": False}))
    json.loads(g.recv())
    h.send(env("user.search", {"keyword": gacct}))
    r = json.loads(h.recv())
    check("searchable off not found", r["data"]["code"] == 0
          and all(u["account"] != gacct for u in r["data"]["data"]["users"]))
    # 关添加：request 403
    g.send(env("settings.set", {"allowAdd": False}))
    json.loads(g.recv())
    h.send(env("contact.request", {"account": gacct}))
    r = json.loads(h.recv())
    check("allow_add off 403", r["type"] == "contact.request_res" and r["data"]["code"] == 403)
    # 恢复（不影响清理）
    g.close(); h.close()
```
注：`gacct` 需在 main() 顶部定义（`G = base + "11"`），用例中直接引用。

- [ ] **步骤 5：清理列表补充新账号**

在清理段 `used` 列表追加新增账号：`D/E/F/G/H`（或 `base+"8"`..`base+"12"`）。

- [ ] **步骤 6：运行内存模式回归**

运行：`bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'`
预期：全部 PASS，用例数从 72 增至 ~95（`72 tests, 0 failures` 附近）。

- [ ] **步骤 7：Commit**

```bash
git add scripts/biz_test.py
git commit -m "test: 联系人/搜索/设置/自己对话框落库 biz 用例（含隐私开关 403/404）"
```

---

## 任务 9：文档更新（v1.8）

**文件：**
- 修改：`docs/protocol.md`
- 修改：`docs/client_api.md`
- 修改：`docs/server-architecture-learning-guide.md`
- 同步：`docs/docs/` 副本

- [ ] **步骤 1：protocol.md 增加 v1.8 版本记录**

在版本变更列表顶部插入：
```markdown
- v1.8 **联系人体系与隐私设置**：新增 `user.search`（按账号/昵称模糊搜索，受 `searchable` 控制）；`contact.request/accept/reject/list/requests/delete`（好友请求状态机，同意后双方成为联系人；`contact.request` 在线推送、离线登录补发）；`settings.get/set` 三开关（`allowTempChat` 允许非联系人临时对话、`searchable` 可被搜索、`allowAdd` 可被添加）；`msg.send` 权限：自己对话框/联系人/目标开启临时对话才可发送，否则 `403 对方未开启临时对话`；**自己对话框统一为 `to==自己 account`（删除 `to:0` 特例），消息落库、历史可查**
```

- [ ] **步骤 2：protocol.md 指令表/事件表/约束更新**

- §4 指令表新增 `user.search`、`contact.request/accept/reject/list/requests/delete`、`settings.get/set` 行
- §5 事件表新增 `contact.request`（`{from, fromNick, ts}`）、`contact.added`（`{account, username}`）、`contact.deleted`（`{account}`）、`user.search_res`、`contact.*_res`、`settings.*_res`
- §4 约束：`msg.send.to` 说明改为 v1.8 语义（删除 `to:0` 自己对话框描述，改为 `to==自己 account`）；补充三开关与 403/409 错误码
- §1 用户模型：说明 `account` 同时用于联系人定位

- [ ] **步骤 3：client_api.md 更新**

- §2 用户模型表：`account` 增加"联系人/搜索/添加"用途说明
- §5 新增子节：`user.search`、`contact.request/accept/reject/list/requests/delete`、`settings.get/set`（含请求/响应示例与错误码）
- §5.4 `msg.send`：`to` 改 v1.8 语义（`to==自己 account` 自己对话框，落库；非联系人且对方未开临时对话 → 403）
- §8.5：搜索→添加→同意→对话流程
- §7 错误码表：新增 403（未开启临时对话/未开启添加）、409（已是联系人/请求已存在）

- [ ] **步骤 4：learning-guide 更新**

- §12 指令路由表：新增 9 个指令行（onSearch/onContact*/onSettings*）
- 新增 ContactRepository 模块说明（§2 模块表或对应章节）

- [ ] **步骤 5：同步 docs/docs/**

运行：`cp docs/protocol.md docs/client_api.md docs/server-architecture-learning-guide.md docs/docs/`

- [ ] **步骤 6：Commit**

```bash
git add docs/protocol.md docs/client_api.md docs/server-architecture-learning-guide.md
git commit -m "docs: v1.8 联系人体系与隐私设置协议/客户端/架构文档"
```

---

## 任务 10：全量回归与提交

**文件：** 无新改动（仅验证）

- [ ] **步骤 1：内存模式全量**

运行：
```bash
bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
bash scripts/test_origin.sh 2>&1 | tail -1
(cd build && ctest 2>&1 | grep 'tests passed')
```
预期：biz 全 PASS（~95）、origin OK、CTest 4/4。

- [ ] **步骤 2：Redis 模式全量**

运行：
```bash
IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380 bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
```
预期：全 PASS（联系人/设置走 MySQL，token/限流/幂等走 Redis，双后端共存验证）。

- [ ] **步骤 3：专项手工验证（可选但推荐）**

用 python 脚本验证：自己对话框落库（发后 chat.history 可查、不回显 msg.recv）、联系人推送（在线/离线补发）、临时对话 403、搜索开关过滤。参考任务 8 用例逻辑。

- [ ] **步骤 4：确认无残留进程**

运行：`pkill -x imserver; sleep 0.5; pgrep -x imserver || echo "no server"`

- [ ] **步骤 5：最终提交（如需合并 docs 已各自 commit）**

确认 `git status` 无未提交的 src/CMakeLists 改动。

---

## 自检

- **规格覆盖度**：三开关（Q1/Q3/Q7/Q8）→ 任务 3/6/8/9；联系人全流程（Q2/Q4/Q6/Q9）→ 任务 2/6/8；自己对话框落库+to==自己（Q10/Q11）→ 任务 4/5/8；历史/已读不限（Q5）→ 任务 5 步骤 5/6；文档 → 任务 9。全部覆盖。
- **占位符扫描**：无 TODO/待定。
- **类型一致性**：`ContactRepository::PendingRequest` 在 contact_repository.hpp 定义、appserver 用 `r.fromUser/r.ts`；`resolveTarget` 返回约定 -1/-2/-3/>0 在任务 4 统一；`fetchSettings` 三出参指针签名在任务 3/6 一致。