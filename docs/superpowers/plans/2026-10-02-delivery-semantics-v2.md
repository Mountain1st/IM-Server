# 投递确认语义 v2（P0.5）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 把“服务端已发送”与“客户端已确认收到”分离——单聊 `status` 恒 PENDING、由 `msg.offline_ack` 推进；群聊改用 `group_members` 成员级 `(ts,id)` 严格游标、由新增 `group.offline_ack` 累计推进。一次性消除整体审查 #1/#2/#3。

**架构：** 落库恒 PENDING → 尝试投递 msg.recv → 客户端 ACK → 服务端推进“已投递”事实。单聊复用 `messages.status`（零迁移）；群聊新增 `group_members.last_delivered_id`，游标按 `(last_delivered_ts, last_delivered_id)` 字典序严格只进。

**技术栈：** C++17、MySQL 8（C API 预处理语句）、nlohmann/json、Python 集成测试（scripts/biz_test.py）。

**规格：** `docs/superpowers/specs/2026-10-02-delivery-semantics-v2-design.md`（三条约束：cumulative ACK / 严格单调 / 账号级，以规格为准）

**工作分支：** `feat/delivery-v2`（从 master 创建；每任务 commit）

**构建/测试环境（执行者必读）：**
- 构建：`IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4`（build 已配置好，勿删）
- 单测：`IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build`（基线 13/13）
- 本地 MySQL 已运行（127.0.0.1:3306，`IMS_jack`/`520010`，库 `IMS_reactor_db`）；本地 MinIO 已运行（127.0.0.1:9000，bucket media）
- biz 全量：
  ```bash
  pkill -x imserver; sleep 1
  cd /home/jackx/IMs && export IM_MYSQL_PASSWORD=520010 IM_MYSQL_HOST=127.0.0.1 IM_MYSQL_PORT=3306 \
    IM_S3_ENDPOINT=127.0.0.1:9000 IM_S3_REGION=us-east-1 IM_S3_BUCKET=media \
    IM_S3_ACCESS_KEY=minioadmin IM_S3_SECRET_KEY=minioadmin IM_S3_PATH_STYLE=true \
    IM_OFFLINE_PURGE_INTERVAL=2 IM_REGISTER_LIMIT_PER_WINDOW=100000
  setsid nohup ./build/imserver 9191 > /tmp/imserver.log 2>&1 < /dev/null & sleep 2
  python3 scripts/biz_test.py   # 基线 337/0
  ```
- 中文注释/commit；真实用户 id1/2 不可动；客户端测试脚本 `send()` 会**消费并返回**响应，勿重复 recv
- **本重构会改变现有 biz 用例的隐含假设**：多处 `.recv()` 消费 `msg.recv` 后未 ACK 也无妨（不 ACK 只影响下次登录补发）；但“离线补发”类既有用例依赖旧“在线即 delivered”，需按任务 5 调整

---

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `db/migrate_delivery_cursor.sql` | 创建 | `group_members` 增 `last_delivered_id`（幂等） |
| `db/schema.sql` | 修改 | 同步 `last_delivered_id` 列 |
| `src/group_repository.hpp/.cpp` | 修改 | `getDeliveredCursor`/`advanceDeliveredCursor`（替代 watermark 语义） |
| `src/message_repository.hpp` | 修改 | `fetchGroupMessages` 增 `sinceId` 参数 |
| `src/mysql_message_repository.hpp/.cpp` | 修改 | 双游标 SQL；`fetchMessageCursor`（ACK 反查） |
| `src/app_message.cpp` | 修改 | 单聊恒 PENDING；补发走 (ts,id)；`onOfflineAck` 泛化；新增 `onGroupOfflineAck`；删群补发 setWatermark |
| `src/app_group.cpp` | 修改 | 删在线投递 `setWatermark`；历史调用传 sinceId=0 |
| `src/appserver.hpp` | 修改 | `onGroupOfflineAck` 声明 |
| `src/appserver.cpp` | 修改 | 分发注册 `group.offline_ack` |
| `scripts/biz_test.py` | 修改 | 投递语义回归段（11 用例） |
| `docs/protocol.md` / `docs/client_api.md` | 修改 | v3.3 协议 |

---

### 任务 1：迁移 + schema

**文件：**
- 创建：`db/migrate_delivery_cursor.sql`
- 修改：`db/schema.sql`（`group_members` 表，约 86-95 行）

- [ ] **步骤 1：创建 `db/migrate_delivery_cursor.sql`**

```sql
-- 迁移：投递确认语义 v2（P0.5）——group_members 增有序投递游标 id
-- 背景：群投递游标从秒级 last_delivered_ts 升级为 (last_delivered_ts, last_delivered_id)
-- 严格有序游标，由客户端 group.offline_ack 累计推进（只进不退）。
-- 存量行默认 0（不追补历史；新逻辑仅从部署后按 ACK 严格推进）。
-- 幂等：information_schema 判定后加列，可重复执行。
-- 用法：MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_delivery_cursor.sql

SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='group_members'
             AND column_name='last_delivered_id');
SET @s := IF(@c = 0,
  'ALTER TABLE group_members ADD COLUMN last_delivered_id BIGINT UNSIGNED NOT NULL DEFAULT 0',
  'SELECT 1');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
```

- [ ] **步骤 2：`schema.sql` 同步**——`group_members` 表 `last_delivered_ts` 行之后加：

```sql
  last_delivered_id BIGINT UNSIGNED NOT NULL DEFAULT 0, -- 有序投递游标 id（v3.3，与 last_delivered_ts 组成严格游标）
```

并把该行原注释 `-- 离线补发水位线` 改为 `-- 投递游标 ts 分量（v3.3：客户端 ACK 推进）`。

- [ ] **步骤 3：本地执行 + 幂等验证**

```bash
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -P 3306 IMS_reactor_db < db/migrate_delivery_cursor.sql
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -P 3306 IMS_reactor_db -e "SHOW COLUMNS FROM group_members LIKE 'last_delivered_id';"
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -P 3306 IMS_reactor_db < db/migrate_delivery_cursor.sql
```

预期：输出 `last_delivered_id | bigint unsigned | NO | | 0`；第二次执行无错误。

- [ ] **步骤 4：Commit**

```bash
git add db/migrate_delivery_cursor.sql db/schema.sql
git commit -m "feat: group_members 增有序投递游标 last_delivered_id（投递语义v2迁移）"
```

---

### 任务 2：存储层——群游标 + 双游标补发查询 + ACK 反查

**文件：**
- 修改：`src/group_repository.hpp:54-56`、`src/group_repository.cpp`（`getWatermark`/`setWatermark` 实现约 428-463 行）
- 修改：`src/message_repository.hpp:112-117`（`fetchGroupMessages` 签名）
- 修改：`src/mysql_message_repository.hpp`（声明）
- 修改：`src/mysql_message_repository.cpp`（`fetchGroupMessages` 实现约 370-395 行；新增 `fetchMessageCursor`）

- [ ] **步骤 1：`group_repository.hpp` 替换水位线接口**（54-56 行）：

```cpp
  // ---- 投递游标（v3.3，语义=客户端已确认收到的位置，账号级）----
  // 返回 (ts, id) 游标分量（无行返回 0,0）。txId 出参：last_delivered_ts / last_delivered_id
  void getDeliveredCursor(int64_t groupId, int64_t userId, int64_t* ts, int64_t* id);
  // 严格只进推进：仅当新游标 (ts,id) 按字典序 > 旧游标才更新（幂等，ACK 重复/回退无副作用）。
  // 返回 true=成功写库或无需更新；false=DB 错误。
  bool advanceDeliveredCursor(int64_t groupId, int64_t userId, int64_t ts, int64_t id);
```

- [ ] **步骤 2：`group_repository.cpp` 替换实现**（把 `getWatermark`/`setWatermark` 两个函数整体替换）：

```cpp
void GroupRepository::getDeliveredCursor(int64_t groupId, int64_t userId, int64_t* ts,
                                         int64_t* id) {
  if (ts) *ts = 0;
  if (id) *id = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return;
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT last_delivered_ts, last_delivered_id FROM group_members "
      "WHERE group_id = ? AND user_id = ? LIMIT 1",
      {std::to_string(groupId), std::to_string(userId)}, &errNo);
  if (!stmt) return;
  int64_t vts = 0, vid = 0;
  MYSQL_BIND res[2];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &vts;
  res[1].buffer_type = MYSQL_TYPE_LONGLONG;
  res[1].buffer = &vid;
  if (!mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
      mysql_stmt_fetch(stmt) == 0) {
    if (ts) *ts = vts;
    if (id) *id = vid;
  }
  mysql_stmt_close(stmt);
}

bool GroupRepository::advanceDeliveredCursor(int64_t groupId, int64_t userId, int64_t ts,
                                             int64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok()) return false;
  // 严格只进：仅当 (newTs,newId) 字典序 > 旧游标才更新。
  // SQL 内比较，单语句原子，免读改写 TOCTOU；重复/回退 ACK 命中 0 行=幂等成功。
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE group_members SET last_delivered_ts = ?, last_delivered_id = ? "
      "WHERE group_id = ? AND user_id = ? AND "
      "((last_delivered_ts < ?) OR (last_delivered_ts = ? AND last_delivered_id < ?))",
      {std::to_string(ts), std::to_string(id), std::to_string(groupId),
       std::to_string(userId), std::to_string(ts), std::to_string(ts),
       std::to_string(id)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 推进群投递游标失败 errno=" << errNo);
    return false;
  }
  mysql_stmt_close(stmt);
  return true;
}
```

- [ ] **步骤 3：`message_repository.hpp` 改 `fetchGroupMessages` 签名**（112-117 行）：

```cpp
  // 拉取某群消息：游标 (sinceTs, sinceId) 严格序（ts > sinceTs 或 ts==sinceTs 且 id>sinceId）；
  // untilTs==0 表示无上界；asc=true 按 (ts,id) 升序（补发），false 倒序（历史）；limit 条。
  virtual std::vector<StoredMessage> fetchGroupMessages(int64_t groupId,
                                                        int64_t sinceTs,
                                                        int64_t sinceId,
                                                        int64_t untilTs,
                                                        int limit, bool asc) = 0;
```

- [ ] **步骤 4：`mysql_message_repository.hpp` 同步签名 + 新增 ACK 反查声明**：

```cpp
  std::vector<StoredMessage> fetchGroupMessages(int64_t groupId, int64_t sinceTs,
                                                int64_t sinceId, int64_t untilTs,
                                                int limit, bool asc) override;
  // 按 message_id 反查 (ts, id, group_id)：群投递 ACK 游标推进用。
  // 找到返回 true（写出 ts/id/groupId）；不存在返回 false。
  bool fetchMessageCursor(const std::string& messageId, int64_t* ts, int64_t* id,
                          int64_t* groupId);
```

- [ ] **步骤 5：`mysql_message_repository.cpp` 改 `fetchGroupMessages` 实现 + 新增 `fetchMessageCursor`**：

`fetchGroupMessages` 整体替换为（注意 SQL 双游标与排序同构）：

```cpp
std::vector<StoredMessage> MySqlMessageRepository::fetchGroupMessages(
    int64_t groupId, int64_t sinceTs, int64_t sinceId, int64_t untilTs, int limit,
    bool asc) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoredMessage> out;
  if (!client_.ok() || groupId <= 0) return out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  // 游标为 (ts,id) 严格序，与排序 (ts,id) 完全同构（防“查询排序≠ACK游标排序”边界 bug）
  std::string sql =
      "SELECT message_id, from_user, to_user, content, ts, client_msg_id, chat_type, status, "
      "group_id, msg_type, media_id FROM messages WHERE group_id = " +
      std::to_string(groupId) + " AND (ts > " + std::to_string(sinceTs) +
      " OR (ts = " + std::to_string(sinceTs) + " AND id > " + std::to_string(sinceId) + "))";
  if (untilTs > 0) sql += " AND ts <= " + std::to_string(untilTs);
  sql += asc ? " ORDER BY ts ASC, id ASC LIMIT " : " ORDER BY ts DESC, id DESC LIMIT ";
  sql += std::to_string(limit);
  if (!client_.query(sql.c_str())) {
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

bool MySqlMessageRepository::fetchMessageCursor(const std::string& messageId, int64_t* ts,
                                                int64_t* id, int64_t* groupId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || messageId.empty()) return false;
  int errNo = 0;
  // messageId 为客户端输入 → 预处理语句防注入
  MYSQL_STMT* stmt = client_.stmtExec(
      "SELECT ts, id, group_id FROM messages WHERE message_id = ? LIMIT 1",
      {messageId}, &errNo);
  if (!stmt) {
    LOG_E("[mysql] 反查消息游标失败 errno=" << errNo);
    return false;
  }
  int64_t vts = 0, vid = 0, vgid = 0;
  MYSQL_BIND res[3];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG; res[0].buffer = &vts;
  res[1].buffer_type = MYSQL_TYPE_LONGLONG; res[1].buffer = &vid;
  res[2].buffer_type = MYSQL_TYPE_LONGLONG; res[2].buffer = &vgid;
  const bool ok = !mysql_stmt_bind_result(stmt, res) && !mysql_stmt_store_result(stmt) &&
                  mysql_stmt_fetch(stmt) == 0;
  mysql_stmt_close(stmt);
  if (ok) {
    if (ts) *ts = vts;
    if (id) *id = vid;
    if (groupId) *groupId = vgid;
  }
  return ok;
}
```

- [ ] **步骤 6：构建（预期因调用点未改而编译失败——这是 TDD 的“失败”点，记录错误行）**

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -E "error:" | head -10
```

预期：报 `fetchGroupMessages` 调用点参数数量不匹配（app_message.cpp/app_group.cpp）——任务 3 修复。

- [ ] **步骤 7：临时修调用点以通过编译（最小改动，任务 3 再语义化）**——`app_message.cpp` 与 `app_group.cpp` 中 `fetchGroupMessages(...)` 调用各加一个 `0` 作为 `sinceId`：把 `fetchGroupMessages(gid, X, Y, Z, true)` 改为 `fetchGroupMessages(gid, X, 0, Y, Z, true)`（after sinceTs）。构建：

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"
```

预期：`0` 与 `13/13`。

- [ ] **步骤 8：Commit**

```bash
git add src/group_repository.hpp src/group_repository.cpp src/message_repository.hpp src/mysql_message_repository.hpp src/mysql_message_repository.cpp src/app_message.cpp src/app_group.cpp
git commit -m "feat: 存储层投递游标——(ts,id) 严格游标 get/advance+双游标补发查询+ACK反查"
```

---

### 任务 3：单聊改确认制 + 群补发去 setWatermark

**文件：**
- 修改：`src/app_message.cpp`（`onSend` 落库 status；`pushOfflineMessages` 群段；`onOfflineAck`）

- [ ] **步骤 1：`onSend` 单聊恒 PENDING**——`app_message.cpp` 约 385-390 行，把：

```cpp
  const bool targetOnline = !onlineMgr_.sessions(to).empty();
  const int status = targetOnline ? kMsgDelivered : kMsgOfflinePending;
```

改为：

```cpp
  // 投递确认语义 v2：一律先落 PENDING，由客户端 msg.offline_ack 推进为 delivered
  // （在线也如此——“服务端已发送”≠“客户端已确认收到”）。targetOnline 仅决定是否即时推送。
  const bool targetOnline = !onlineMgr_.sessions(to).empty();
  const int status = kMsgOfflinePending;
```

（自己对话框分支 `app_message.cpp:368-380` 落 `kMsgDelivered` 保持不变——无 ACK 来源。）

- [ ] **步骤 2：`pushOfflineMessages` 群段改双游标 + 去 setWatermark**——把群循环（约 84-99 行）改为：

```cpp
    const int64_t wmTs = 0, wmId = 0;  // 占位，见下
    int64_t cts = 0, cid = 0;
    groupRepo_->getDeliveredCursor(gid, userId, &cts, &cid);
    // 群补发游标 = max(会话游标, 入群前一刻)——入群前消息不可见（joined-1 使同秒可见）
    const int64_t sinceTs = std::max(cts, joined - 1);
    const int64_t sinceId = (sinceTs == cts) ? cid : 0;
    auto grows = msgRepo_->fetchGroupMessages(gid, sinceTs, sinceId, left, 100, true);
    std::string gname;
    groupRepo_->getGroup(gid, &gname, nullptr);
    for (const auto& m : grows) {
      if (m.fromUser == userId) continue;      // 跳过自身消息（与在线路径一致）
      if (m.status == kMsgRecalled) continue;  // 已撤回不补发
      arr.push_back(messageToJson(m, gname));
      maxTs = std::max(maxTs, m.ts);
      // 注意：不在此处推进游标——由客户端 group.offline_ack 确认后推进（v3.2→v3.3 关键修复）
    }
```

（删除原 `const int64_t wm = ...; auto grows = ...(std::max(wm, joined-1), left, 100, true);` 与循环内 `groupRepo_->setWatermark(gid, userId, m.ts);`。删掉未用的 `wmTs/wmId` 占位行。）

- [ ] **步骤 3：`onOfflineAck` 语义泛化**（函数体，约 430-449 行）——替换为：

```cpp
void AppServer::onOfflineAck(const std::shared_ptr<Session>& session, const json& data) {
  // 通用投递确认（v3.3）：单聊在线/离线通用。服务端把对应消息 status 0→1（幂等）。
  // 注意：群消息不走此指令（群 to_user=0，经 group.offline_ack 推进成员游标）。
  if (data.contains("messageIds") && data["messageIds"].is_array() &&
      data["messageIds"].size() > kMaxOfflineAckIds) {
    session->reply(makeResult("msg.offline_ack_res", 400, "确认数量超限", json::object()));
    return;
  }
  std::vector<std::string> messageIds;
  if (data.contains("messageIds") && data["messageIds"].is_array()) {
    for (const auto& v : data["messageIds"]) {
      if (v.is_string()) messageIds.push_back(v.get<std::string>());
    }
  }
  msgRepo_->markDelivered(session->userId(), messageIds);
  session->reply(makeResult("msg.offline_ack_res", 0, "ok", json::object()));
}
```

（逻辑与现状一致，仅注释语义化；`markDelivered` 谓词 `to_user=? AND status=0` 现在真正生效——因为在线也置 0。）

- [ ] **步骤 4：构建 + 单测**

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"
```

预期：`0` 与 `13/13`。

- [ ] **步骤 5：Commit**

```bash
git add src/app_message.cpp
git commit -m "feat: 单聊恒PENDING由ACK推进；群补发移除读即推进(改客户端确认)"
```

---

### 任务 4：群 ACK 处理 + 接线

**文件：**
- 修改：`src/appserver.hpp`（声明）
- 修改：`src/appserver.cpp`（分发注册）
- 修改：`src/app_group.cpp`（`onGroupOfflineAck` 实现）

- [ ] **步骤 1：`appserver.hpp` 声明**（`onGroupHistory` 附近）：

```cpp
  void onGroupOfflineAck(const std::shared_ptr<Session>& session, const json& data);
```

- [ ] **步骤 2：`appserver.cpp` 分发注册**（`{"group.history", ...}` 之后）：

```cpp
    {"group.offline_ack", &AppServer::onGroupOfflineAck},
```

- [ ] **步骤 3：`app_group.cpp` 实现 `onGroupOfflineAck`**（`onGroupHistory` 之后）：

```cpp
void AppServer::onGroupOfflineAck(const std::shared_ptr<Session>& session, const json& data) {
  // 群投递确认（v3.3）：cumulative ACK——客户端声明“截至该 (ts,messageId) 的连续前缀已收到”。
  // 服务端把该成员投递游标严格只进推进到该位置（重复/回退幂等）。账号级语义（任一设备确认即推进）。
  const char* kRes = "group.offline_ack_res";
  if (!data.contains("groupId") || !data["groupId"].is_number_integer() ||
      !data.contains("ts") || !data["ts"].is_number_integer() ||
      !data.contains("messageId") || !data["messageId"].is_string()) {
    session->reply(makeResult(kRes, 400, "参数错误", json::object()));
    return;
  }
  const int64_t groupId = data["groupId"].get<int64_t>();
  const int64_t ts = data["ts"].get<int64_t>();
  const std::string messageId = data["messageId"].get<std::string>();
  if (groupId <= 0 || ts <= 0 || messageId.empty()) {
    session->reply(makeResult(kRes, 400, "参数错误", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, session->userId())) {
    session->reply(makeResult(kRes, 403, "非群成员", json::object()));
    return;
  }
  int64_t mts = 0, mid = 0, mgid = 0;
  if (!msgRepo_->fetchMessageCursor(messageId, &mts, &mid, &mgid) || mgid != groupId) {
    // 统一 404：消息不存在或不属于该群（不过度泄露细节）
    session->reply(makeResult(kRes, 404, "消息不存在或不属于该群", json::object()));
    return;
  }
  // 以反查所得 (mts,mid) 为候选游标（不信任客户端 ts），严格只进推进（幂等）
  if (!groupRepo_->advanceDeliveredCursor(groupId, session->userId(), mts, mid)) {
    session->reply(makeResult(kRes, 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult(kRes, 0, "ok", json::object()));
}
```

- [ ] **步骤 4：构建 + 单测**

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"
```

预期：`0` 与 `13/13`。

- [ ] **步骤 5：手工冒烟**（单聊确认制 + 群 ACK）：起服务后脚本覆盖——A→B 在线，B 收 msg.recv **不 ACK**；B 重登收到 msg.offline 含该消息；B `msg.offline_ack` 后重登不含。群：A 群发，成员 B 在线收；B 不 ACK 重登补；B `group.offline_ack` 后重登不补；重复 ACK 幂等。冒烟脚本放 `/tmp/opencode/` 执行后删除，账号精确清理。

```bash
pkill -x imserver; sleep 1
cd /home/jackx/IMs && export IM_MYSQL_PASSWORD=520010 IM_MYSQL_HOST=127.0.0.1 IM_MYSQL_PORT=3306 \
  IM_OFFLINE_PURGE_INTERVAL=2 IM_REGISTER_LIMIT_PER_WINDOW=100000
setsid nohup ./build/imserver 9191 > /tmp/imserver.log 2>&1 < /dev/null & sleep 2
# 运行你的冒烟脚本（参考 recall/conversation 段的 WS 用法）
```

- [ ] **步骤 6：Commit**

```bash
git add src/appserver.hpp src/appserver.cpp src/app_group.cpp
git commit -m "feat: group.offline_ack 累计确认路由（成员校验+反查(ts,id)+严格只进推进，幂等）"
```

---

### 任务 5：biz 回归测试（11 用例）

**文件：**
- 修改：`scripts/biz_test.py`（在 conversation 段之后、`v1.close(); v2.close(); v3.close()` 之后、`a.close(); b.close()` 之前插入；账号 94/95/96/97 加入清理列表）

**注意：** 本任务可能暴露任务 3/4 的语义缺口。失败先查 `/tmp/imserver.log` 定位根因（可能是产品代码 bug），修根因，勿绕过断言。

- [ ] **步骤 1：插入投递语义测试段**：

```python
    # ================= 投递确认语义 v2（delivery ack）=================
    # 独立账号；d 前缀避免与既有 clientMsgId 撞车。
    D1, D2, D3 = base + "94", base + "95", base + "96"
    d1 = WS(); d2 = WS(); d3 = WS()
    for w, acct in ((d1, D1), (d2, D2), (d3, D3)):
        assert register(w, acct, "pw1234")["data"]["code"] == 0
        assert login(w, acct, "pw1234")["data"]["code"] == 0
    d1.send(env("contact.request", {"account": D2})); json.loads(d1.recv())
    d2.send(env("contact.accept", {"account": D1})); json.loads(d2.recv())
    json.loads(d1.recv()); json.loads(d2.recv())
    time.sleep(1.1)

    # 1. 单聊在线未 ACK → 重登补发（在线即 PENDING）
    r = send_msg(d1, D2, "在线未确认", "ds1-" + base)
    check("dlv single online send ok", r["data"]["code"] == 0)
    r = json.loads(d2.recv())
    check("dlv single online recv", r["type"] == "msg.recv")
    mid1 = r["data"]["messageId"]
    d2x = WS()
    assert login(d2x, D2, "pw1234")["data"]["code"] == 0
    off = json.loads(d2x.recv())   # 重登应补发未 ACK 的在线消息
    check("dlv single unacked re-delivered", off["type"] == "msg.offline"
          and any(m["messageId"] == mid1 for m in off["data"]["msgs"]))
    # ACK 后重登不再补
    r = send(d2x, "msg.offline_ack", {"messageIds": [mid1]})
    check("dlv single ack ok", r["data"]["code"] == 0)
    d2y = WS()
    assert login(d2y, D2, "pw1234")["data"]["code"] == 0
    got = None
    try:
        d2y.s.settimeout(1.5); got = json.loads(d2y.recv())
    except Exception:
        got = None
    d2y.s.settimeout(5)
    if got is not None and got["type"] == "msg.offline":
        check("dlv single acked not re-delivered",
              all(m["messageId"] != mid1 for m in got["data"]["msgs"]))
    else:
        check("dlv single acked not re-delivered", True)
    # 2. ACK 重复幂等
    r = send(d2x, "msg.offline_ack", {"messageIds": [mid1]})
    check("dlv single ack idempotent", r["data"]["code"] == 0)
    d2x.close(); d2y.close()

    # 3. 群在线未 ACK → 重登补发
    r = send(d1, "group.create", {"name": "投递群"})
    check("dlv group create ok", r["data"]["code"] == 0)
    dgid = r["data"]["data"]["groupId"]
    r = send(d1, "group.invite", {"groupId": dgid, "account": D2})
    check("dlv group invite ok", r["data"]["code"] == 0)
    json.loads(d1.recv()); json.loads(d2.recv())
    time.sleep(1.1)
    r = send_msg(d1, dgid, "群在线未确认", "dg1-" + base, chat_type="group")
    check("dlv group send ok", r["data"]["code"] == 0)
    g1 = json.loads(d2.recv())
    check("dlv group online recv", g1["type"] == "msg.recv" and g1["data"]["groupId"] == dgid)
    d2z = WS()
    assert login(d2z, D2, "pw1234")["data"]["code"] == 0
    off = json.loads(d2z.recv())
    check("dlv group unacked re-delivered",
          off["type"] == "msg.offline"
          and any(m.get("messageId") == g1["data"]["messageId"] for m in off["data"]["msgs"]))
    # 4. 群 ACK 后重登不补 + 幂等 + 只进不退
    r = send(d2z, "group.offline_ack", {"groupId": dgid, "ts": g1["data"]["ts"],
                                        "messageId": g1["data"]["messageId"]})
    check("dlv group ack ok", r["data"]["code"] == 0)
    r = send(d2z, "group.offline_ack", {"groupId": dgid, "ts": g1["data"]["ts"],
                                        "messageId": g1["data"]["messageId"]})
    check("dlv group ack idempotent", r["data"]["code"] == 0)
    d2w = WS()
    assert login(d2w, D2, "pw1234")["data"]["code"] == 0
    got = None
    try:
        d2w.s.settimeout(1.5); got = json.loads(d2w.recv())
    except Exception:
        got = None
    d2w.s.settimeout(5)
    if got is not None and got["type"] == "msg.offline":
        check("dlv group acked not re-delivered",
              all(m.get("messageId") != g1["data"]["messageId"] for m in got["data"]["msgs"]))
    else:
        check("dlv group acked not re-delivered", True)
    # 5. 群 ACK 越权/非法
    r = send(d3, "group.offline_ack", {"groupId": dgid, "ts": g1["data"]["ts"],
                                       "messageId": g1["data"]["messageId"]})
    check("dlv group ack non-member 403", r["data"]["code"] == 403)
    r = send(d2z, "group.offline_ack", {"groupId": dgid, "ts": 1, "messageId": "no-such"})
    check("dlv group ack notfound 404", r["data"]["code"] == 404)
    r = send(d2z, "group.offline_ack", {"groupId": dgid})
    check("dlv group ack bad params 400", r["data"]["code"] == 400)
    # 6. 同秒多条群消息不丢：连发 3 条（同秒），ACK 第 2 条后重登只补第 3 条
    same = []
    for i in range(3):
        r = send_msg(d1, dgid, "同秒%d" % i, "dgs-%d-%s" % (i, base), chat_type="group")
        check("dlv same-sec send %d" % i, r["data"]["code"] == 0)
        same.append(json.loads(d2z.recv())["data"])
    # ACK 第 2 条（连续前缀末端）
    r = send(d2z, "group.offline_ack",
             {"groupId": dgid, "ts": same[1]["ts"], "messageId": same[1]["messageId"]})
    check("dlv same-sec ack 2nd ok", r["data"]["code"] == 0)
    d2v = WS()
    assert login(d2v, D2, "pw1234")["data"]["code"] == 0
    off = json.loads(d2v.recv())
    mids = [m.get("messageId") for m in off.get("data", {}).get("msgs", [])] \
        if off.get("type") == "msg.offline" else []
    check("dlv same-sec only last backfilled",
          same[2]["messageId"] in mids and same[0]["messageId"] not in mids
          and same[1]["messageId"] not in mids)
    # 7. 撤回的 pending 单聊消息不再补发
    r = send_msg(d1, D2, "待撤回", "dr1-" + base)
    r = json.loads(d2v.recv())
    rmid = r["data"]["messageId"]
    r = send(d1, "msg.recall", {"messageId": rmid})
    check("dlv recall own pending ok", r["data"]["code"] == 0)
    json.loads(d2v.recv())   # recall_notify
    d2u = WS()
    assert login(d2u, D2, "pw1234")["data"]["code"] == 0
    got = None
    try:
        d2u.s.settimeout(1.5); got = json.loads(d2u.recv())
    except Exception:
        got = None
    d2u.s.settimeout(5)
    found = got is not None and got.get("type") == "msg.offline" \
        and any(m["messageId"] == rmid for m in got["data"]["msgs"])
    check("dlv recalled pending not re-delivered", not found)
    d1.close(); d2.close(); d3.close(); d2z.close(); d2v.close(); d2u.close()
```

**补充用例 8（自身消息被游标跨过 / 服务端流位置 ≠ 可交付前缀）**——追加在用例 7 之后、`d1.close(); d2.close(); ...` 之前：

```python
    # 8. 自身消息被游标跨过：A 在线发 X（自身）后离线，B 发 Y；A 重登仅补 Y（X 跳过），ACK Y 越过 X
    r = send_msg(d1, dgid, "own-X", "dox-" + base, chat_type="group")
    check("dlv own X send ok", r["data"]["code"] == 0)
    gx = json.loads(d2.recv())   # B 在线收到 A 的 X
    check("dlv B recv own X", gx["type"] == "msg.recv")
    d1.close()                   # A 离线（d1 的投递游标尚未推进：自身消息从不投递给发送端）
    time.sleep(1.1)
    r = send_msg(d2, dgid, "peer-Y", "doy-" + base, chat_type="group")
    check("dlv B send Y ok", r["data"]["code"] == 0)
    d1b = WS()
    assert login(d1b, D1, "pw1234")["data"]["code"] == 0
    off = json.loads(d1b.recv())
    omsgs = off.get("data", {}).get("msgs", []) if off.get("type") == "msg.offline" else []
    mids8 = [m.get("messageId") for m in omsgs]
    gy = [m for m in omsgs if m.get("clientMsgId") == "doy-" + base]
    check("dlv own message skipped in backfill", gx["data"]["messageId"] not in mids8)
    check("dlv peer message backfilled", len(gy) == 1)
    r = send(d1b, "group.offline_ack", {"groupId": dgid, "ts": gy[0]["ts"],
                                        "messageId": gy[0]["messageId"]})
    check("dlv ack crosses own message", r["data"]["code"] == 0)
    d1b.close()
```

同时把 `D1, D2, D3` 追加进清理列表 `used = [...]`。

- [ ] **步骤 2：重启服务 + 全量 biz**

```bash
pkill -x imserver; sleep 1
cd /home/jackx/IMs && export IM_MYSQL_PASSWORD=520010 IM_MYSQL_HOST=127.0.0.1 IM_MYSQL_PORT=3306 \
  IM_S3_ENDPOINT=127.0.0.1:9000 IM_S3_REGION=us-east-1 IM_S3_BUCKET=media \
  IM_S3_ACCESS_KEY=minioadmin IM_S3_SECRET_KEY=minioadmin IM_S3_PATH_STYLE=true \
  IM_OFFLINE_PURGE_INTERVAL=2 IM_REGISTER_LIMIT_PER_WINDOW=100000
setsid nohup ./build/imserver 9191 > /tmp/imserver.log 2>&1 < /dev/null & sleep 2
python3 scripts/biz_test.py 2>&1 | tail -6
```

预期：`~359 tests, 0 failures`（337 基线 + 投递语义段）。若既有用例因“在线不再立即 delivered”而回归失败，按规格修正**测试预期**（不是产品代码）——例如既有“离线补发”用例若依赖在线消息不补发，需在其后补 ACK 或调整断言。逐条记录在报告。

- [ ] **步骤 3：DB 无残留验证**

```bash
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -P 3306 IMS_reactor_db -N \
  -e "SELECT COUNT(*) FROM users; SELECT COUNT(*) FROM messages;"
```

预期：users=2。

- [ ] **步骤 4：Commit**

```bash
git add scripts/biz_test.py
git commit -m "test: 投递语义v2 回归——单聊ACK制/群累计ACK/幂等/越权/同秒不丢/撤回抑制/多端账号级"
```

---

### 任务 6：文档 v3.3

**文件：** `docs/protocol.md`、`docs/client_api.md`

- [ ] **步骤 1：protocol.md 版本行**——标题 v3.2→v3.3，版本变更列表加：

```markdown
- v3.3 **投递确认语义 v2**：统一“服务端已发送 ≠ 客户端已确认收到 ≠ 已读”。单聊消息落库恒 `PENDING`，由 `msg.offline_ack`（扩展为通用投递确认，在线/离线通用）推进为 `DELIVERED`；群聊新增 `group.offline_ack`——**累计 ACK**（声明“截至该 (ts,messageId) 连续前缀已收到”），服务端按 `group_members.(last_delivered_ts,last_delivered_id)` **严格只进**推进该成员投递游标（幂等）。游标为**用户级**（任一设备确认即推进，非设备级）。ACK 丢失→下次登录补发（at-least-once，客户端按 messageId 去重）。见 §5 指令表与字段细则
```

- [ ] **步骤 2：protocol.md 指令表加 `group.offline_ack` 请求行 + 响应行**（沿用既有表格格式）：

请求：
```markdown
| `group.offline_ack` | `{ "groupId": 1, "ts": 1720000000, "messageId": "..." }` | 群投递累计确认（v3.3）：客户端声明已连续收到该 `(ts,messageId)` 及之前可见窗口内的全部群消息；服务端严格只进推进该成员投递游标（重复/回退幂等）。回 `group.offline_ack_res` |
```

响应：
```markdown
| `group.offline_ack_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 群投递确认结果（v3.3）：`0` 成功（含幂等）；`400` 参数错误、`403` 非群成员、`404` 消息不存在或不属于该群、`500` 服务器内部错误 |
```

- [ ] **步骤 3：protocol.md `msg.offline_ack` 行补充语义**（把描述改为“通用投递确认”）：

```markdown
| `msg.offline_ack` | `{ "messageIds": ["...", ...] }` | 通用投递确认（v3.3，在线/离线通用）：客户端确认已收到对应单聊消息，服务端标记 `0→1`（幂等）；回 `msg.offline_ack_res`。群消息确认用 `group.offline_ack` |
```

- [ ] **步骤 4：protocol.md 字段细则加三条约束**（撤回占位符条目之后）：

```markdown
- **投递确认三条约束（v3.3）**：① `group.offline_ack` 为**累计 ACK**——语义是“截至该 (ts,messageId) 的连续前缀均已收到”，客户端**必须只对连续前缀末端发送**（乱序补洞后才 ACK 更高位置），否则空洞消息将被服务端累计确认而永久跳过；② 服务端游标按 `(ts,内部id)` **严格单调只进**（`max(旧,新)` 字典序），重复/回退 ACK 幂等；③ 游标为**账号级非设备级**——任一在线设备确认即推进该用户游标，落后设备重登不补已确认段（与单聊 delivered 语义一致）
- **单聊投递状态（v3.3）**：`status` 由 `0 PENDING` 起步，仅客户端 `msg.offline_ack` 后可 `1 DELIVERED`；在线即时收到但未 ACK 的消息仍会在下次登录补发（at-least-once，客户端按 messageId 去重）
```

- [ ] **步骤 5：client_api.md**——标题 v3.2→v3.3；新增 §5.34 `group.offline_ack`（请求/响应/错误码/累计语义/客户端连续前缀义务）；§5.x `msg.offline_ack` 说明改为通用投递确认；§6 补 `group.offline_ack_res`。

- [ ] **步骤 6：Commit**

```bash
git add docs/protocol.md docs/client_api.md
git commit -m "docs: 协议 v3.3 投递确认语义 v2（单聊ACK制/群累计ACK/严格游标/账号级）"
```

---

### 任务 7：终验 + 合并

- [ ] **步骤 1：全量验证**

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"   # 0
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"           # 13/13
# biz 全量（环境见任务 5 步骤 2）→ 0 failures
git grep -n "jackx\|520010" -- src db | head -3   # 预期无（biz_test 的 MYSQL_PWD 变量除外）
```

- [ ] **步骤 2：合并 master**

```bash
git checkout master && git merge --no-ff feat/delivery-v2 -m "Merge branch 'feat/delivery-v2': 投递确认语义v2（单聊ACK制+群累计ACK严格游标）——收敛#1/#2/#3"
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"
```

- [ ] **步骤 3：推送 + 账本**

```bash
git push origin master
```

（推送属用户已授权范围；CI 由用户在网页确认。）更新 `.superpowers/sdd/progress.md`。

---

## 自检记录

1. **规格覆盖度**：§3 单聊→T3；§4 群游标（4.1 cumulative/4.2 严格/4.3 账号级）→T2（advance）+T4（校验/反查/语义）+T6（文档）；§5 查询同构→T2 步骤 5；§6 协议→T4+T6；§7 改动清单→T1-T4；§8 测试 11 用例→T5；§9 取舍→T6 文档。无遗漏。
2. **占位符扫描**：T3 步骤 2 中 `wmTs/wmId` 标注为“占位，见下”并在同步骤要求删除——非 TODO，是替换过程的过渡标记，已注明删除。其余步骤均含完整代码。
3. **类型一致性**：`getDeliveredCursor(gid,uid,&ts,&id)`/`advanceDeliveredCursor(gid,uid,ts,id)` T2 定义、T3（补发）/T4（ACK）使用一致；`fetchGroupMessages(gid,sinceTs,sinceId,untilTs,limit,asc)` T2 定义、T3/T4/历史使用一致；`fetchMessageCursor(messageId,&ts,&id,&groupId)` T2 定义、T4 使用一致；`group.offline_ack` 字段 T4 实现、T5 断言、T6 文档一致。
