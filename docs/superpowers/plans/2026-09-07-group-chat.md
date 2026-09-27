# 群聊最小闭环（v1.9）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 实现群聊最小闭环（建群/拉人/群发消息/群历史/退群），单行存储 + 水位线离线补发。

**架构：** 新增 groups/group_members 表与 GroupRepository（成员集合内存缓存）；messages 表加 group_id 列，群消息单行存储；AppServer 新增 app_group.cpp（6 指令）+ onSend 群分支 + 离线补发合并；单聊三查询加 `AND group_id = 0` 隔离。

**技术栈：** C++17、MySQL（libmysqlclient）、现有 TEST/CHECK 单测宏、biz_test.py 集成测试。

**规格：** `docs/superpowers/specs/2026-09-07-group-chat-design.md`

**测试基线：** `bash scripts/test_biz.sh` 内存 105/105、`IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380` Redis 105/105、`bash scripts/test_origin.sh`、`cd build && ctest` 6/6。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `db/schema.sql`（改） | groups/group_members 建表 + messages 加 group_id 列与索引 |
| `src/group_repository.hpp`（新增） | GroupRepository 接口：群/成员/水位线 |
| `src/group_repository.cpp`（新增） | MySQL 实现 + 成员集合内存缓存（仿 ContactRepository） |
| `src/message_repository.hpp`（改） | StoredMessage 加 groupId；saveMessage 加 groupId 参数（默认 0）；新增 fetchGroupMessages |
| `src/mysql_message_repository.cpp`（改） | group_id 列读写 + 单聊三查询加隔离 + 群查询实现 |
| `src/appserver.hpp`（改） | 群 handler 声明 + groupRepo_ 成员 |
| `src/appserver.cpp`（改） | 路由 6 个群指令 + 构造 groupRepo_ |
| `src/app_group.cpp`（新增） | 6 个群 handler |
| `src/app_message.cpp`（改） | onSend 群分支 + pushOfflineMessages 合并群行 + chatType 放行 group |
| `CMakeLists.txt`（改） | 加 group_repository.cpp + app_group.cpp |
| `scripts/biz_test.py`（改） | 群测试段 + chatType 非法值测试替换 + 清理段扩展 |
| `docs/protocol.md`、`docs/client_api.md`（改） | v1.9 条目 + 群指令/事件表 |

---

### 任务 1：数据库 schema（群表 + messages.group_id）

**文件：**
- 修改：`db/schema.sql`

- [ ] **步骤 1：schema.sql 追加建表语句**

在 `friend_requests` 建表语句之后追加（完整内容）：
```sql
CREATE TABLE IF NOT EXISTS groups (
  group_id    BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name        VARCHAR(32)  NOT NULL,
  owner_id    BIGINT UNSIGNED NOT NULL,
  created_ts  BIGINT       NOT NULL,
  KEY idx_owner (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS group_members (
  id                BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_id          BIGINT UNSIGNED NOT NULL,
  user_id           BIGINT UNSIGNED NOT NULL,
  joined_ts         BIGINT       NOT NULL,
  left_ts           BIGINT       NOT NULL DEFAULT 0,  -- 0=在群，非0=离开时间
  last_delivered_ts BIGINT       NOT NULL DEFAULT 0,  -- 离线补发水位线
  UNIQUE KEY uk_group_user (group_id, user_id),
  KEY idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

- [ ] **步骤 2：messages 表加 group_id 列与索引**

修改 `messages` 建表语句：在 `chat_type` 行之后加 `group_id BIGINT UNSIGNED NOT NULL DEFAULT 0,`，在 `KEY idx_message_id (message_id)` 行之后加 `KEY idx_group_ts (group_id, ts),`。

- [ ] **步骤 3：现库执行迁移**

运行（开发库已有 messages 表，需 ALTER；IF NOT EXISTS 不适用于 ALTER，按顺序执行）：
```bash
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db -e "
CREATE TABLE IF NOT EXISTS groups (
  group_id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name VARCHAR(32) NOT NULL,
  owner_id BIGINT UNSIGNED NOT NULL,
  created_ts BIGINT NOT NULL,
  KEY idx_owner (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE TABLE IF NOT EXISTS group_members (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_id BIGINT UNSIGNED NOT NULL,
  user_id BIGINT UNSIGNED NOT NULL,
  joined_ts BIGINT NOT NULL,
  left_ts BIGINT NOT NULL DEFAULT 0,
  last_delivered_ts BIGINT NOT NULL DEFAULT 0,
  UNIQUE KEY uk_group_user (group_id, user_id),
  KEY idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
ALTER TABLE messages ADD COLUMN group_id BIGINT UNSIGNED NOT NULL DEFAULT 0;
ALTER TABLE messages ADD KEY idx_group_ts (group_id, ts);"
```
预期：无报错（若 group_id 列已存在则 ALTER 报错属预期，跳过该行重跑其余）。

- [ ] **步骤 4：验证表结构**

运行：`MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -e "USE IMS_reactor_db; SHOW TABLES; SHOW COLUMNS FROM messages;" 2>/dev/null | grep -E 'groups|group_members|group_id'`
预期：输出含 `groups`、`group_members`、`group_id` 三行。

- [ ] **步骤 5：Commit**

```bash
git add db/schema.sql
git commit -m "feat: 群聊表结构（groups/group_members + messages.group_id 索引）"
```

---

### 任务 2：GroupRepository 接口与实现

**文件：**
- 创建：`src/group_repository.hpp`
- 创建：`src/group_repository.cpp`

- [ ] **步骤 1：编写 group_repository.hpp（完整内容）**

```cpp
#pragma once

#include "mysql_client.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace im {

// 群组/成员/水位线的 MySQL 存储。
// 仿 ContactRepository：单连接 + 内部互斥锁串行化 DB 访问。
// isMember 用内存缓存（members_ 集合，仅 left_ts==0 的现成员）加速发送热路径，
// 启动全量加载；加人/退群时同步刷新缓存。
class GroupRepository {
 public:
  GroupRepository(const std::string& host, unsigned port, const std::string& user,
                  const std::string& password, const std::string& db);

  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  // ---- 群 ----
  // 建群，返回 groupId（>0），失败返回 0。
  int64_t createGroup(const std::string& name, int64_t ownerId, int64_t now);
  bool groupExists(int64_t groupId);
  // 查询群信息，找到返回 true（写出 name/ownerId）。
  bool getGroup(int64_t groupId, std::string* name, int64_t* ownerId);

  // ---- 成员 ----
  bool isMember(int64_t groupId, int64_t userId);  // 现成员（走缓存）
  // 成员关系（含退群）：有行返回 true（写出 joinedTs/leftTs，leftTs==0 表示在群）。
  bool memberPeriod(int64_t groupId, int64_t userId, int64_t* joinedTs, int64_t* leftTs);
  // 加人（新加或重入：重入时 joined_ts=now、left_ts=0）。成功返回 true。
  bool addMember(int64_t groupId, int64_t userId, int64_t now);
  // 退群（left_ts=now，行保留）。成功返回 true。
  bool removeMember(int64_t groupId, int64_t userId, int64_t now);
  size_t activeCount(int64_t groupId);                    // 现成员数
  std::vector<int64_t> listMemberIds(int64_t groupId);    // 现成员 userId 列表
  std::vector<int64_t> listUserGroups(int64_t userId);    // 某用户所在现群 groupId 列表

  // ---- 水位线 ----
  int64_t getWatermark(int64_t groupId, int64_t userId);
  void setWatermark(int64_t groupId, int64_t userId, int64_t ts);

 private:
  void loadMembersCache();

  MySqlClient client_;
  std::mutex mutex_;  // 串行化 DB 访问 + 缓存读写
  // 现成员关系缓存；isMember 查 "gid|uid"
  std::unordered_set<std::string> members_;  // 键格式 "gid|uid"
};

}  // namespace im
```

- [ ] **步骤 2：编写 group_repository.cpp（完整内容）**

```cpp
#include "group_repository.hpp"

#include "log.hpp"

#include <cstdlib>
#include <cstring>

namespace im {

GroupRepository::GroupRepository(const std::string& host, unsigned port,
                                 const std::string& user, const std::string& password,
                                 const std::string& db)
    : client_(host, port, user, password, db) {
  loadMembersCache();
}

void GroupRepository::loadMembersCache() {
  if (!client_.ok()) return;
  MYSQL* c = client_.handle();
  if (mysql_query(c, "SELECT group_id, user_id FROM group_members WHERE left_ts = 0")) {
    LOG_E("[mysql] 加载群成员缓存失败: " << client_.errStr());
    return;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0] && row[1]) {
      members_.emplace(std::string(row[0]) + "|" + row[1]);
    }
  }
  mysql_free_result(res);
  LOG_I("[mysql] 已加载 " << members_.size() << " 条群成员关系到缓存");
}

int64_t GroupRepository::createGroup(const std::string& name, int64_t ownerId,
                                      int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(), "INSERT INTO groups (name, owner_id, created_ts) VALUES (?, ?, ?)",
      {name, std::to_string(ownerId), std::to_string(now)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 建群失败 errno=" << errNo);
    return 0;
  }
  const int64_t gid = static_cast<int64_t>(mysql_stmt_insert_id(stmt));
  mysql_stmt_close(stmt);
  if (gid <= 0) return 0;
  // 建群者自动入群
  MYSQL_STMT* s2 = mysqlStmtExec(
      client_.handle(),
      "INSERT INTO group_members (group_id, user_id, joined_ts) VALUES (?, ?, ?)",
      {std::to_string(gid), std::to_string(ownerId), std::to_string(now)}, &errNo);
  if (!s2) {
    LOG_E("[mysql] 建群者入群失败 errno=" << errNo);
    return 0;
  }
  mysql_stmt_close(s2);
  members_.emplace(std::to_string(gid) + "|" + std::to_string(ownerId));
  return gid;
}

bool GroupRepository::groupExists(int64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || groupId <= 0) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
                                   "SELECT 1 FROM groups WHERE group_id = ? LIMIT 1",
                                   {std::to_string(groupId)}, &errNo);
  if (!stmt) return false;
  int one = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONG;
  res[0].buffer = &one;
  bool found = false;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt)) {
    found = (mysql_stmt_fetch(stmt) == 0);
  }
  mysql_stmt_close(stmt);
  return found;
}

bool GroupRepository::getGroup(int64_t groupId, std::string* name, int64_t* ownerId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || groupId <= 0) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(), "SELECT name, owner_id FROM groups WHERE group_id = ? LIMIT 1",
      {std::to_string(groupId)}, &errNo);
  if (!stmt) return false;
  char nameBuf[128] = {0};
  unsigned long nameLen = 0;
  bool nameNull = false;
  int64_t oid = 0;
  MYSQL_BIND res[2];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_STRING;
  res[0].buffer = nameBuf;
  res[0].buffer_length = sizeof nameBuf;
  res[0].length = &nameLen;
  res[0].is_null = &nameNull;
  res[1].buffer_type = MYSQL_TYPE_LONGLONG;
  res[1].buffer = &oid;
  bool found = false;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
      mysql_stmt_fetch(stmt) == 0) {
    if (name) *name = nameNull ? std::string() : std::string(nameBuf, nameLen);
    if (ownerId) *ownerId = oid;
    found = true;
  }
  mysql_stmt_close(stmt);
  return found;
}

bool GroupRepository::isMember(int64_t groupId, int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return members_.count(std::to_string(groupId) + "|" + std::to_string(userId)) != 0;
}

bool GroupRepository::memberPeriod(int64_t groupId, int64_t userId, int64_t* joinedTs,
                                   int64_t* leftTs) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT joined_ts, left_ts FROM group_members WHERE group_id = ? AND user_id = ? LIMIT 1",
      {std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) return false;
  int64_t j = 0, l = 0;
  MYSQL_BIND res[2];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &j;
  res[1].buffer_type = MYSQL_TYPE_LONGLONG;
  res[1].buffer = &l;
  bool found = false;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
      mysql_stmt_fetch(stmt) == 0) {
    if (joinedTs) *joinedTs = j;
    if (leftTs) *leftTs = l;
    found = true;
  }
  mysql_stmt_close(stmt);
  return found;
}

bool GroupRepository::addMember(int64_t groupId, int64_t userId, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  // 新加或重入：重入时 joined_ts=now、left_ts=0（旧时段自然不可见）
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "INSERT INTO group_members (group_id, user_id, joined_ts) VALUES (?, ?, ?) "
      "ON DUPLICATE KEY UPDATE joined_ts = VALUES(joined_ts), left_ts = 0",
      {std::to_string(groupId), std::to_string(userId), std::to_string(now)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 群加人失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  members_.emplace(std::to_string(groupId) + "|" + std::to_string(userId));
  return true;
}

bool GroupRepository::removeMember(int64_t groupId, int64_t userId, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "UPDATE group_members SET left_ts = ? WHERE group_id = ? AND user_id = ? "
      "AND left_ts = 0",
      {std::to_string(now), std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 群退群失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  members_.erase(std::to_string(groupId) + "|" + std::to_string(userId));
  return true;
}

size_t GroupRepository::activeCount(int64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  const std::string sql = "SELECT COUNT(*) FROM group_members WHERE group_id = " +
                          std::to_string(groupId) + " AND left_ts = 0";
  if (mysql_query(client_.handle(), sql.c_str())) return 0;
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return 0;
  size_t n = 0;
  if (MYSQL_ROW row = mysql_fetch_row(res)) {
    if (row[0]) n = static_cast<size_t>(std::strtoull(row[0], nullptr, 10));
  }
  mysql_free_result(res);
  return n;
}

std::vector<int64_t> GroupRepository::listMemberIds(int64_t groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int64_t> out;
  if (!client_.ok()) return out;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT user_id FROM group_members WHERE group_id = ? AND left_ts = 0",
      {std::to_string(groupId)}, &errNo);
  if (!stmt) return out;
  int64_t uid = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &uid;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt)) {
    while (mysql_stmt_fetch(stmt) == 0) out.push_back(uid);
  }
  mysql_stmt_close(stmt);
  return out;
}

std::vector<int64_t> GroupRepository::listUserGroups(int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int64_t> out;
  if (!client_.ok()) return out;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT group_id FROM group_members WHERE user_id = ? AND left_ts = 0",
      {std::to_string(userId)}, &errNo);
  if (!stmt) return out;
  int64_t gid = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &gid;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt)) {
    while (mysql_stmt_fetch(stmt) == 0) out.push_back(gid);
  }
  mysql_stmt_close(stmt);
  return out;
}

int64_t GroupRepository::getWatermark(int64_t groupId, int64_t userId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return 0;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "SELECT last_delivered_ts FROM group_members WHERE group_id = ? AND user_id = ? "
      "LIMIT 1",
      {std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) return 0;
  int64_t v = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &v;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt) ||
      mysql_stmt_fetch(stmt) != 0) {
    v = 0;
  }
  mysql_stmt_close(stmt);
  return v;
}

void GroupRepository::setWatermark(int64_t groupId, int64_t userId, int64_t ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(
      client_.handle(),
      "UPDATE group_members SET last_delivered_ts = ? WHERE group_id = ? AND user_id = ?",
      {std::to_string(ts), std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 更新群水位线失败 errno=" << errNo);
    return;
  }
  mysql_stmt_close(stmt);
}

}  // namespace im
```

- [ ] **步骤 3：语法编译验证**

运行：`g++ -std=c++17 -Isrc -Ithird_party $(mysql_config --cflags 2>/dev/null || echo "-I/usr/include/mysql") -c src/group_repository.cpp -o /tmp/gr.o 2>&1 | head -10`
预期：无输出（仅语法验证；正式集成见任务 4 的 CMake）。

- [ ] **步骤 4：Commit**

```bash
git add src/group_repository.hpp src/group_repository.cpp
git commit -m "feat: GroupRepository 群/成员/水位线存储（含成员缓存）"
```

---

### 任务 3：消息存储层扩展（group_id + 群查询 + 单聊隔离）

**文件：**
- 修改：`src/message_repository.hpp`
- 修改：`src/mysql_message_repository.cpp`
- 修改：`src/mysql_message_repository.hpp`（saveMessage 声明同步）

- [ ] **步骤 1：StoredMessage 加 groupId + saveMessage 加参**

`src/message_repository.hpp` 的 StoredMessage 追加字段：
```cpp
  int status;             // 0=offline_pending 1=delivered 2=read
  int64_t groupId = 0;    // 0=单聊；>0=群聊（群消息单行：from=发送者, to=0）
```
saveMessage 声明改为（末尾默认参数，老调用点零改动）：
```cpp
  virtual bool saveMessage(const std::string& messageId, int64_t fromUser,
                           int64_t toUser, const std::string& content,
                           int64_t ts, const std::string& clientMsgId,
                           const std::string& chatType, int status,
                           int64_t groupId = 0) = 0;
```
新增群查询接口：
```cpp
  // 拉取某群在时间窗内的消息：ts > sinceTs（水位线），untilTs==0 表示无上界；
  // asc=true 按 ts 升序（补发），false 按 ts 倒序（历史）；limit 条。
  virtual std::vector<StoredMessage> fetchGroupMessages(int64_t groupId,
                                                        int64_t sinceTs,
                                                        int64_t untilTs,
                                                        int limit, bool asc) = 0;
```

- [ ] **步骤 2：mysql 实现同步**

`src/mysql_message_repository.hpp` 的 saveMessage 声明同步加 `int64_t groupId = 0`；cpp 实现：
- `kMsgCols` 8→9；`rowToMessage` 追加 `m.groupId = std::strtoll(row[8], nullptr, 10);`
- saveMessage SQL 加 `group_id` 列（9 个 `?`，参数末尾加 `std::to_string(groupId)`）
- fetchOffline / fetchHistory 的 SELECT 列表追加 `group_id` 列；fetchOffline 加 `AND group_id = 0`；fetchHistory 加 `AND group_id = 0`（WHERE 条件内，两处 OR 分支整体与 group 条件 AND）
- markRead 加 `AND group_id = 0`
- 新增 fetchGroupMessages 实现：
```cpp
std::vector<StoredMessage> MySqlMessageRepository::fetchGroupMessages(
    int64_t groupId, int64_t sinceTs, int64_t untilTs, int limit, bool asc) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoredMessage> out;
  if (!client_.ok() || groupId <= 0) return out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  std::string sql =
      "SELECT message_id, from_user, to_user, content, ts, client_msg_id, chat_type, status, "
      "group_id FROM messages WHERE group_id = " + std::to_string(groupId) +
      " AND ts > " + std::to_string(sinceTs);
  if (untilTs > 0) sql += " AND ts <= " + std::to_string(untilTs);
  sql += asc ? " ORDER BY ts ASC, id ASC LIMIT " : " ORDER BY ts DESC, id DESC LIMIT ";
  sql += std::to_string(limit);
  if (mysql_query(client_.handle(), sql.c_str())) {
    LOG_E("[mysql] 查询群消息失败: " << client_.errStr());
    return out;
  }
  MYSQL_RES* res = mysql_store_result(client_.handle());
  if (!res) return out;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    unsigned long* lens = mysql_fetch_lengths(res);
    out.push_back(rowToMessage(row, lens));
  }
  mysql_free_result(res);
  return out;
}
```
并在 `src/mysql_message_repository.hpp` 追加该方法声明（含 override）。

- [ ] **步骤 3：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出（saveMessage 默认参数保证老调用点通过）。

- [ ] **步骤 4：Commit**

```bash
git add src/message_repository.hpp src/mysql_message_repository.hpp src/mysql_message_repository.cpp
git commit -m "feat: 消息存储支持 group_id（群查询 + 单聊隔离）"
```

---

### 任务 4：AppServer 集成（app_group + 路由 + 发送分支 + 离线合并）

**文件：**
- 修改：`src/appserver.hpp`（群 handler 声明 + groupRepo_ 成员）
- 修改：`src/appserver.cpp`（路由 6 指令 + 构造 groupRepo_）
- 创建：`src/app_group.cpp`（6 个群 handler）
- 修改：`src/app_message.cpp`（onSend 群分支 + pushOfflineMessages 合并群行 + chatType 放行）
- 修改：`CMakeLists.txt`（加 group_repository.cpp + app_group.cpp）

- [ ] **步骤 1：appserver.hpp 加声明与成员**

在 `onSettingsSet` 声明后追加：
```cpp
  void onGroupCreate(const std::shared_ptr<Session>& session, const json& data);
  void onGroupInvite(const std::shared_ptr<Session>& session, const json& data);
  void onGroupLeave(const std::shared_ptr<Session>& session, const json& data);
  void onGroupList(const std::shared_ptr<Session>& session, const json& data);
  void onGroupMembers(const std::shared_ptr<Session>& session, const json& data);
  void onGroupHistory(const std::shared_ptr<Session>& session, const json& data);
```
在 `contactRepo_` 成员行后追加：
```cpp
  std::unique_ptr<GroupRepository> groupRepo_;  // 群/成员/水位线存储
```
顶部加 `#include "group_repository.hpp"`。

- [ ] **步骤 2：appserver.cpp 路由 + 构造**

onEnvelope 中 `settings.set` 分支后追加 6 个路由（完整代码）：
```cpp
    if (type == "group.create") {
      onGroupCreate(session, data);
      return;
    }
    if (type == "group.invite") {
      onGroupInvite(session, data);
      return;
    }
    if (type == "group.leave") {
      onGroupLeave(session, data);
      return;
    }
    if (type == "group.list") {
      onGroupList(session, data);
      return;
    }
    if (type == "group.members") {
      onGroupMembers(session, data);
      return;
    }
    if (type == "group.history") {
      onGroupHistory(session, data);
      return;
    }
```
构造 contactRepo_ 初始化后追加 groupRepo_ 初始化 + ok 校验（仿 contactRepo_ 块）：
```cpp
      groupRepo_(std::make_unique<GroupRepository>(
          userConfig.mysqlHost, userConfig.mysqlPort, userConfig.mysqlUser,
          userConfig.mysqlPassword, userConfig.mysqlDb)),
```
```cpp
  if (!groupRepo_->ok()) {
    throw std::runtime_error("群存储连接失败: " + groupRepo_->errStr());
  }
  std::cout << "[db] 群存储: MySQL (host=" << userConfig.mysqlHost << ":"
            << userConfig.mysqlPort << ", db=" << userConfig.mysqlDb << ")" << std::endl;
```
（注：该 cout 在任务“日志迁移”后应为 LOG_I；本分支基线已是 LOG_*，新代码直接用 LOG_I。）

- [ ] **步骤 3：创建 src/app_group.cpp（完整内容）**

```cpp
#include "appserver.hpp"
#include "app_util.hpp"
#include "log.hpp"
#include "session.hpp"

#include <ctime>

namespace im {

namespace {
constexpr size_t kMaxGroupMembers = 50;      // 群人数上限（含群主）
constexpr size_t kMaxGroupNameLen = 32;      // 群名长度上限
}  // namespace

void AppServer::onGroupCreate(const std::shared_ptr<Session>& session, const json& data) {
  std::string name = data.value("name", "");
  if (name.empty() || name.size() > kMaxGroupNameLen) {
    session->reply(makeResult("group.create_res", 400, "参数错误", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int64_t gid = groupRepo_->createGroup(name, session->userId(), now);
  if (gid <= 0) {
    session->reply(makeResult("group.create_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("group.create_res", 0, "ok",
                            json{{"groupId", gid}, {"name", name}}));
}

void AppServer::onGroupInvite(const std::shared_ptr<Session>& session, const json& data) {
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.invite_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId)) {
    session->reply(makeResult("group.invite_res", 404, "群不存在", json::object()));
    return;
  }
  if (ownerId != session->userId()) {
    session->reply(makeResult("group.invite_res", 403, "仅群主可拉人", json::object()));
    return;
  }
  const std::string account = data.value("account", "");
  int64_t target = 0;
  std::string nick;
  const int frc = userStore_->lookupAccount(account, &target, &nick);
  if (frc == 2) {
    session->reply(makeResult("group.invite_res", 500, "服务器内部错误", json::object()));
    return;
  }
  if (frc != 0) {
    session->reply(makeResult("group.invite_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (!contactRepo_->isContact(session->userId(), target)) {
    session->reply(makeResult("group.invite_res", 409, "对方不是你的联系人", json::object()));
    return;
  }
  if (groupRepo_->isMember(groupId, target)) {
    session->reply(makeResult("group.invite_res", 409, "对方已在群中", json::object()));
    return;
  }
  if (groupRepo_->activeCount(groupId) >= kMaxGroupMembers) {
    session->reply(makeResult("group.invite_res", 403, "群已满", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (!groupRepo_->addMember(groupId, target, now)) {
    session->reply(makeResult("group.invite_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("group.invite_res", 0, "ok", json::object()));
  // 推送被拉人（在线）；离线靠 group.list + msg.offline 自洽
  std::string inviterAccount;
  userStore_->accountOf(session->userId(), &inviterAccount);
  json added = {{"groupId", groupId}, {"name", name}, {"inviter", inviterAccount}};
  for (auto& t : onlineMgr_.sessions(target)) {
    t->reply(makeEnvelopeText("group.added", added));
  }
  // 通知群内在线成员
  std::string targetAccount;
  userStore_->accountOf(target, &targetAccount);
  json changed = {{"groupId", groupId}, {"change", "join"}, {"account", targetAccount}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    if (uid == target) continue;
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.member_changed", changed));
    }
  }
}

void AppServer::onGroupLeave(const std::shared_ptr<Session>& session, const json& data) {
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.leave_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId)) {
    session->reply(makeResult("group.leave_res", 404, "群不存在", json::object()));
    return;
  }
  if (ownerId == session->userId()) {
    session->reply(makeResult("group.leave_res", 403, "群主不能退群", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    session->reply(makeResult("group.leave_res", 403, "非群成员", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  groupRepo_->removeMember(groupId, session->userId(), now);
  session->reply(makeResult("group.leave_res", 0, "ok", json::object()));
  // 通知剩余在线成员
  std::string account;
  userStore_->accountOf(session->userId(), &account);
  json changed = {{"groupId", groupId}, {"change", "leave"}, {"account", account}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.member_changed", changed));
    }
  }
}

void AppServer::onGroupList(const std::shared_ptr<Session>& session, const json& data) {
  const auto gids = groupRepo_->listUserGroups(session->userId());
  json arr = json::array();
  for (const int64_t gid : gids) {
    std::string name;
    int64_t ownerId = 0;
    if (!groupRepo_->getGroup(gid, &name, &ownerId)) continue;
    arr.push_back(json{{"groupId", gid},
                       {"name", name},
                       {"memberCount", groupRepo_->activeCount(gid)},
                       {"isOwner", ownerId == session->userId()}});
  }
  session->reply(makeResult("group.list_res", 0, "ok", json{{"groups", arr}}));
}

void AppServer::onGroupMembers(const std::shared_ptr<Session>& session, const json& data) {
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.members_res", 400, "参数错误", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    session->reply(makeResult("group.members_res", 403, "非群成员", json::object()));
    return;
  }
  const auto ids = groupRepo_->listMemberIds(groupId);
  json arr = json::array();
  for (const int64_t uid : ids) {
    std::string account, username;
    int64_t joined = 0, left = 0;
    userStore_->accountOf(uid, &account);
    userStore_->nickname(uid, &username);
    groupRepo_->memberPeriod(groupId, uid, &joined, nullptr);
    std::string gname;
    int64_t ownerId = 0;
    groupRepo_->getGroup(groupId, &gname, &ownerId);
    arr.push_back(json{{"account", account},
                       {"username", username},
                       {"joinedTs", joined},
                       {"isOwner", uid == ownerId}});
  }
  session->reply(makeResult("group.members_res", 0, "ok", json{{"members", arr}}));
}

void AppServer::onGroupHistory(const std::shared_ptr<Session>& session, const json& data) {
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.history_res", 400, "参数错误", json::object()));
    return;
  }
  int64_t joined = 0, left = 0;
  if (!groupRepo_->memberPeriod(groupId, session->userId(), &joined, &left)) {
    session->reply(makeResult("group.history_res", 403, "非群成员", json::object()));
    return;
  }
  int limit = data.value("limit", 50);
  auto stored = msgRepo_->fetchGroupMessages(groupId, joined, left, limit, false);
  std::string gname;
  groupRepo_->getGroup(groupId, &gname, nullptr);
  json arr = json::array();
  for (const auto& m : stored) {
    std::string fromNick, fromAccount;
    userStore_->nickname(m.fromUser, &fromNick);
    if (!userStore_->accountOf(m.fromUser, &fromAccount)) fromAccount.clear();
    arr.push_back(json{{"from", fromAccount},
                       {"fromNick", fromNick},
                       {"groupId", groupId},
                       {"groupName", gname},
                       {"content", m.content},
                       {"ts", m.ts},
                       {"clientMsgId", m.clientMsgId},
                       {"messageId", m.messageId},
                       {"chatType", "group"}});
  }
  session->reply(makeResult("group.history_res", 0, "ok", json{{"msgs", arr}}));
}

}  // namespace im
```
注意：`getGroup(groupId, &gname, nullptr)` 传空 ownerId 指针——GroupRepository::getGroup 实现须判空（任务 2 代码已判空，`if (ownerId)`，一致）。

- [ ] **步骤 4：onSend 群分支 + pushOfflineMessages 合并**

`src/app_message.cpp` 的 chatType 校验（当前）：
```cpp
  // chatType 仅支持单聊；缺省按 "single" 处理（协议 §3）
  std::string chatType = data.value("chatType", "single");
  if (chatType != "single") {
    ack(400, "封包格式错误");
    return;
  }
```
改为：
```cpp
  // chatType：缺省 "single"；"group" 走群分支（协议 v1.9）；其他值拒绝
  std::string chatType = data.value("chatType", "single");
  if (chatType == "group") {
    onGroupSend(session, data, content, clientMsgId);
    return;
  }
  if (chatType != "single") {
    ack(400, "封包格式错误");
    return;
  }
```
在 `onSend` 之前新增 `AppServer::onGroupSend`（放在 app_group.cpp 末尾，声明加入 appserver.hpp 私有区）：
```cpp
void AppServer::onGroupSend(const std::shared_ptr<Session>& session, const json& data,
                            const std::string& content, const std::string& clientMsgId) {
  auto ack = [&](int code, const std::string& msg) {
    session->reply(makeEnvelopeText("msg.ack",
        json{{"code", code}, {"msg", msg}, {"clientMsgId", clientMsgId}}));
  };
  int64_t groupId = 0;
  if (data.contains("to") && data["to"].is_number_integer()) {
    groupId = data["to"].get<int64_t>();
  } else {
    ack(400, "参数错误");
    return;
  }
  std::string gname;
  int64_t ownerId = 0;
  if (!groupRepo_->getGroup(groupId, &gname, &ownerId)) {
    ack(404, "群不存在");
    return;
  }
  // 幂等 + 限流（与单聊同一套，按发送方去重）
  if (msgDedup_->isDuplicate(session->userId(), clientMsgId)) {
    ack(0, "ok");
    return;
  }
  const size_t sendLimit = clientMsgId.empty() ? kMaxSendNoIdPerWindow : kMaxSendPerWindow;
  if (!msgLimiter_->allow(session->userId(), sendLimit, kSendWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    ack(429, "消息发送过于频繁，请稍后再试");
    return;
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    ack(403, "非群成员");
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string messageId = nextMessageId();
  if (!msgRepo_->saveMessage(messageId, session->userId(), 0, content, now, clientMsgId,
                             "group", kMsgDelivered, groupId)) {
    ack(500, "服务器内部错误");
    return;
  }
  msgDedup_->remember(session->userId(), clientMsgId);
  ack(0, "ok");
  // 发送方信息（from 为账号，userId 对客户端不透明）
  std::string fromNick, fromAccount;
  userStore_->nickname(session->userId(), &fromNick);
  if (!userStore_->accountOf(session->userId(), &fromAccount)) fromAccount.clear();
  json recvData = {{"from", fromAccount},
                   {"fromNick", fromNick},
                   {"groupId", groupId},
                   {"groupName", gname},
                   {"content", content},
                   {"ts", now},
                   {"clientMsgId", clientMsgId},
                   {"messageId", messageId},
                   {"chatType", "group"}};
  int64_t maxTs = now;
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    if (uid == session->userId()) continue;  // 发送者不回显
    bool pushed = false;
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("msg.recv", recvData));
      pushed = true;
    }
    if (pushed) {
      groupRepo_->setWatermark(groupId, uid, now);
      g_metrics.messagesDelivered.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_metrics.messagesOffline.fetch_add(1, std::memory_order_relaxed);
    }
  }
}
```
`pushOfflineMessages` 在单聊 `arr` 构建后、`session->reply` 之前插入群行合并（完整代码）：
```cpp
  // 群离线补发：各群按水位线查行（ts > watermark 且在成员在群期间内），与单聊行按 ts 合并
  int64_t maxTs = 0;
  for (const auto& m : stored) maxTs = std::max(maxTs, m.ts);
  for (const int64_t gid : groupRepo_->listUserGroups(userId)) {
    int64_t joined = 0, left = 0;
    if (!groupRepo_->memberPeriod(gid, userId, &joined, &left)) continue;
    const int64_t wm = groupRepo_->getWatermark(gid, userId);
    auto grows = msgRepo_->fetchGroupMessages(gid, std::max(wm, joined - 1), left, 100, true);
    std::string gname;
    groupRepo_->getGroup(gid, &gname, nullptr);
    for (const auto& m : grows) {
      std::string fromNick, fromAccount;
      userStore_->nickname(m.fromUser, &fromNick);
      if (!userStore_->accountOf(m.fromUser, &fromAccount)) fromAccount.clear();
      arr.push_back(json{{"from", fromAccount},
                         {"fromNick", fromNick},
                         {"groupId", gid},
                         {"groupName", gname},
                         {"content", m.content},
                         {"ts", m.ts},
                         {"clientMsgId", m.clientMsgId},
                         {"messageId", m.messageId},
                         {"chatType", "group"}});
      maxTs = std::max(maxTs, m.ts);
      groupRepo_->setWatermark(gid, userId, m.ts);
    }
  }
  if (!arr.empty()) {
    // 按 ts 升序合并（单聊行已升序，群行逐群升序追加后整体排序）
    std::sort(arr.begin(), arr.end(),
              [](const json& a, const json& b) { return a["ts"] < b["ts"]; });
  }
```
注意：`std::max(wm, joined - 1)` 保证 sinceTs 语义为 `ts > since` 且不漏 joined 秒消息；`std::sort` 需 `#include <algorithm>`（app_message.cpp 已有，经任务 6 helper 抽取确认）。

- [ ] **步骤 5：CMakeLists 加源文件**

`src/app_contact.cpp` 行后（或 appserver 相关行处）追加：
```cmake
    src/group_repository.cpp
    src/app_group.cpp
```

- [ ] **步骤 6：编译验证**

运行：`cmake -S . -B build > /dev/null && cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出。

- [ ] **步骤 7：Commit**

```bash
git add src/appserver.hpp src/appserver.cpp src/app_group.cpp src/app_message.cpp CMakeLists.txt
git commit -m "feat: 群聊指令与发送分支（建群/拉人/退群/列表/成员/历史 + 离线合并）"
```

---

### 任务 5：biz 群测试 + 非法 chatType 测试替换

**文件：**
- 修改：`scripts/biz_test.py`

- [ ] **步骤 1：替换 chatType 非法值测试**

biz_test.py 现有（约 366 行）：
```python
    a.send(env("msg.send", {"to": bob, "content": "x", "clientMsgId": "ct2",
                            "chatType": "group"}))
    r = json.loads(a.recv())
    check("chatType group 400", r["type"] == "msg.ack" and r["data"]["code"] == 400)
```
改为：
```python
    a.send(env("msg.send", {"to": bob, "content": "x", "clientMsgId": "ct2",
                            "chatType": "invalid"}))
    r = json.loads(a.recv())
    check("chatType invalid 400", r["type"] == "msg.ack" and r["data"]["code"] == 400)
```

- [ ] **步骤 2：新增群聊测试段**

在联系人测试段之后、清理段之前追加（完整代码；复用 A/B/C 账号与 bob；G* 为 11 位新账号）：
```python
    # ---- v1.9 群聊 ----
    G1 = base + "60"; G2 = base + "61"
    g1 = WS(); register(g1, G1, "pw1234", nick="群友1"); login(g1, G1, "pw1234")
    g2 = WS(); register(g2, G2, "pw1234", nick="群友2"); login(g2, G2, "pw1234")
    # 建群
    a.send(env("group.create", {"name": "测试群"}))
    r = json.loads(a.recv())
    check("group create ok", r["type"] == "group.create_res" and r["data"]["code"] == 0)
    gid = r["data"]["data"]["groupId"]
    # 建群空名 400
    a.send(env("group.create", {"name": ""}))
    r = json.loads(a.recv())
    check("group create empty 400", r["type"] == "group.create_res"
          and r["data"]["code"] == 400)
    # 拉人：bob（联系人）成功；g1（非联系人）409
    a.send(env("group.invite", {"groupId": gid, "account": B}))
    r = json.loads(a.recv())
    check("group invite contact ok", r["type"] == "group.invite_res"
          and r["data"]["code"] == 0)
    r = json.loads(b.recv())
    check("group added pushed", r["type"] == "group.added"
          and r["data"]["groupId"] == gid)
    a.send(env("group.invite", {"groupId": gid, "account": G1}))
    r = json.loads(a.recv())
    check("group invite non-contact 409", r["type"] == "group.invite_res"
          and r["data"]["code"] == 409)
    # 非群主拉人 403（bob 不是群主）
    b.send(env("group.invite", {"groupId": gid, "account": G1}))
    r = json.loads(b.recv())
    check("group invite non-owner 403", r["type"] == "group.invite_res"
          and r["data"]["code"] == 403)
    # 群发消息：a 发，b 收到（from/groupId/chatType 校验），a 不回显
    a.send(env("msg.send", {"to": gid, "content": "hello group", "clientMsgId": "g1",
                            "chatType": "group"}))
    r = json.loads(a.recv())
    check("group send ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    r = json.loads(b.recv())
    check("group recv ok", r["type"] == "msg.recv" and r["data"]["from"] == A
          and r["data"]["groupId"] == gid and r["data"]["chatType"] == "group")
    a.s.settimeout(0.5)
    try:
        echo = a.recv()
    except socket.timeout:
        echo = None
    a.s.settimeout(5)
    check("group no echo to sender", echo is None)
    # 非成员 g1 发送 403
    g1.send(env("msg.send", {"to": gid, "content": "x", "clientMsgId": "g2",
                             "chatType": "group"}))
    r = json.loads(g1.recv())
    check("group send non-member 403", r["type"] == "msg.ack" and r["data"]["code"] == 403)
    # 不存在的群 404
    a.send(env("msg.send", {"to": 999999999, "content": "x", "clientMsgId": "g3",
                            "chatType": "group"}))
    r = json.loads(a.recv())
    check("group send missing 404", r["type"] == "msg.ack" and r["data"]["code"] == 404)
    # 群历史：b 可查，g1（从未加入）403
    b.send(env("group.history", {"groupId": gid}))
    r = json.loads(b.recv())
    check("group history ok", r["type"] == "group.history_res"
          and r["data"]["code"] == 0 and len(r["data"]["data"]["msgs"]) >= 1)
    g1.send(env("group.history", {"groupId": gid}))
    r = json.loads(g1.recv())
    check("group history non-member 403", r["type"] == "group.history_res"
          and r["data"]["code"] == 403)
    # 群列表与成员
    b.send(env("group.list", {}))
    r = json.loads(b.recv())
    check("group list ok", r["type"] == "group.list_res"
          and any(x["groupId"] == gid for x in r["data"]["data"]["groups"]))
    b.send(env("group.members", {"groupId": gid}))
    r = json.loads(b.recv())
    check("group members ok", r["type"] == "group.members_res"
          and len(r["data"]["data"]["members"]) == 2)
    # 退群：b 退群后发消息 403；b 可查在群期间历史；群主 a 退群 403
    b.send(env("group.leave", {"groupId": gid}))
    r = json.loads(b.recv())
    check("group leave ok", r["type"] == "group.leave_res" and r["data"]["code"] == 0)
    b.send(env("msg.send", {"to": gid, "content": "x", "clientMsgId": "g4",
                            "chatType": "group"}))
    r = json.loads(b.recv())
    check("group send after leave 403", r["type"] == "msg.ack" and r["data"]["code"] == 403)
    b.send(env("group.history", {"groupId": gid}))
    r = json.loads(b.recv())
    check("group history ex-member ok", r["type"] == "group.history_res"
          and r["data"]["code"] == 0 and len(r["data"]["data"]["msgs"]) >= 1)
    a.send(env("group.leave", {"groupId": gid}))
    r = json.loads(a.recv())
    check("group owner leave 403", r["type"] == "group.leave_res"
          and r["data"]["code"] == 403)
    g1.close(); g2.close()
```
注意：bob（B）与 alice（A）须已是联系人（联系人测试段在前，D/E 流程不影响 A/B；若 A/B 非联系人则 invite 409——联系人段中 A 与 B 是否建联？biz 现有流程 A/B 为收发测试账号，未显式建联。**本用例要求 A/B 为联系人**，因此在群段开头先建联）：
```python
    # A/B 建联（群邀请要求联系人）
    a.send(env("contact.request", {"account": B}))
    json.loads(a.recv()); json.loads(b.recv())
    b.send(env("contact.accept", {"account": A}))
    json.loads(b.recv()); json.loads(a.recv())
```
将此 5 行插在 `# ---- v1.9 群聊 ----` 之后、建群之前。若 A/B 已是联系人（request 回 409），则跳过 accept——为稳妥用兼容写法：
```python
    a.send(env("contact.request", {"account": B}))
    r = json.loads(a.recv())
    if r["data"]["code"] == 0:
        json.loads(b.recv())
        b.send(env("contact.accept", {"account": A}))
        json.loads(b.recv()); json.loads(a.recv())
```

- [ ] **步骤 3：清理段扩展**

清理段 `used` 列表追加 `G1, G2`（即 `base + "60"`, `base + "61"`），并在 messages 清理后追加群表清理：
```python
    subprocess.run(["bash", "-c",
        "MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -e \""
        "USE IMS_reactor_db; "
        "DELETE FROM group_members WHERE user_id IN (SELECT id FROM users WHERE account IN (%s)); "
        "DELETE FROM groups WHERE owner_id IN (SELECT id FROM users WHERE account IN (%s));\" 2>/dev/null" % (inlist, inlist)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
```
（放在现有 messages/users 清理之后、fails 统计之前；`inlist` 变量已存在可直接复用。）

- [ ] **步骤 4：运行内存模式测试**

运行：`bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'`
预期：全部 PASS，用例数 105 → ~130（`~130 tests, 0 failures`）。

- [ ] **步骤 5：Commit**

```bash
git add scripts/biz_test.py
git commit -m "test: 群聊 biz 用例（建群/拉人/收发/历史/退群/权限矩阵）"
```

---

### 任务 6：文档更新（v1.9）

**文件：**
- 修改：`docs/protocol.md`
- 修改：`docs/client_api.md`
- 同步：`docs/docs/` 副本

- [ ] **步骤 1：protocol.md 增加 v1.9 版本记录**

在版本变更列表顶部插入：
```markdown
- v1.9 **群聊最小闭环**：新增 `group.create/invite/leave/list/members/history`（建群、群主拉人、退群、列表、成员、历史）；`msg.send` 支持 `chatType: "group"`（`to` 为数字 groupId）；群消息单行存储（`to_user=0, group_id=G`），离线按水位线补发；`msg.recv/msg.offline` 群载荷含 `groupId/groupName`；群不做已读；`chat.history`/`msg.read` 单聊视图加群隔离
```

- [ ] **步骤 2：protocol.md 指令表/事件表/约束更新**

- §4 指令表新增 6 行 group 指令（含参数与回包）
- §5 事件表新增 `group.added`（`{groupId, name, inviter}`）、`group.member_changed`（`{groupId, change, account}`）、`group.*_res`
- §4 约束：`msg.send` 的 `chatType` 说明改为 single/group 双分支；三开关与 403/409 错误码补充群场景（非群成员 403、非群主 403、群主退群 403、群已满 403、已在群 409）
- §1 用户模型：说明 groupId 命名空间独立（数字自增，与 userId 无关，客户端按 chatType 区分）

- [ ] **步骤 3：client_api.md 更新**

- §2 用户模型表：`groupId` 行（数字群标识，群收发定位目标）
- §5 新增子节：`group.create/invite/leave/list/members/history`（含请求/响应示例与错误码）
- §5.4 `msg.send`：`chatType: "group"` 时 `to` 为 groupId 的说明 + 群权限
- §8.5：搜索→添加→同意→对话流程后追加建群→拉人→群聊流程

- [ ] **步骤 4：同步 docs/docs/**

运行：`cp docs/protocol.md docs/client_api.md docs/server-architecture-learning-guide.md docs/docs/`
（注：learning-guide 本次仅同步，无新增章节要求；若实现者发现路由表需加 group 行，可顺手在 §12.1 追加 6 行，不属强制。）

- [ ] **步骤 5：Commit**

```bash
git add docs/protocol.md docs/client_api.md
git commit -m "docs: v1.9 群聊协议/客户端文档"
```

---

### 任务 7：全量回归与提交

**文件：** 无新改动（仅验证）

- [ ] **步骤 1：内存模式全量**

运行：
```bash
bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
bash scripts/test_origin.sh 2>&1 | tail -1
(cd build && ctest 2>&1 | grep 'tests passed')
```
预期：biz 全 PASS（~130）、origin OK、CTest 6/6。

- [ ] **步骤 2：Redis 模式全量**

运行：
```bash
IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380 bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
```
预期：全 PASS（群存储走 MySQL，token/限流/幂等走 Redis，双后端共存验证）。

- [ ] **步骤 3：残留检查**

运行：
```bash
pkill -x imserver; sleep 0.5; pgrep -x imserver || echo "no server"
git status --short | grep -vE '^\?\?' || echo "已提交，无未提交改动"
```
预期：`no server`；无未提交改动（各任务已分别 commit）。

---

## 自检

- **规格覆盖度**：群表/成员/水位线（任务 1/2）✓；msg.send 群分支 + 幂等限流复用（任务 4）✓；离线合并 + 水位推进（任务 4）✓；群历史时间过滤（任务 4）✓；6 指令 + 3 推送（任务 4）✓；权限矩阵（成员/群主/50/联系人/重入，任务 4+5）✓；单聊隔离（任务 3）✓；文档 v1.9（任务 6）✓；双模式回归（任务 7）✓。
- **占位符扫描**：无 TODO/待定；任务 5 的 A/B 建联兼容写法为完整代码非占位；任务 4 步骤 7 的 commit 文件清单确切。
- **类型一致性**：`fetchGroupMessages(groupId, sinceTs, untilTs, limit, asc)` 在任务 3 定义、任务 4 调用（补发 `fetchGroupMessages(gid, max(wm,joined-1), left, 100, true)`、历史 `fetchGroupMessages(groupId, joined, left, limit, false)`）一致；`memberPeriod(gid, uid, &joined, &left)` 在任务 2 定义、任务 4 两处调用一致；`getGroup(gid, &name, &ownerId)` 含 nullptr 容忍（任务 2 实现已判空）；`onGroupSend(session, data, content, clientMsgId)` 在任务 4 定义与调用一致；`GroupRepository` 方法名与任务 2 头文件一致。
