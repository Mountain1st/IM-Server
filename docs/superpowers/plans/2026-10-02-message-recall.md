# 消息撤回（msg.recall）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 实现消息撤回 msg.recall（单聊+群聊，发送者 120 秒内），历史/离线出口占位符剥离，媒体可达性隔离，多端 notify 同步。

**架构：** `messages.status` 扩展终态 `3=RECALLED`；单条 UPDATE 原子校验（存在+发送者+时间窗+状态迁移，免 TOCTOU）；四处出站序列化收敛到单一 helper `messageToJson`；`msg.recall_notify` 免 ACK 扇出；`markRead`/`hasMediaAccess` 谓词修正防内容复活与媒体泄露。

**技术栈：** C++17、MySQL 8（C API 预处理语句）、nlohmann/json、Python 集成测试（scripts/biz_test.py）。

**规格：** `docs/superpowers/specs/2026-10-02-message-recall-design.md`（裁定/并发矩阵/安全分析以规格为准）

**工作分支：** `feat/message-recall`（从 master 创建；每个任务结束 commit）

**构建/测试环境（执行者必读）：**
- 构建：`IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4`（build 目录已配置好，勿删）
- 单测：`IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build`（基线 13/13）
- 本地 MySQL 已运行（127.0.0.1:3306，`IMS_jack`/`520010`，库 `IMS_reactor_db`）；本地 MinIO 已运行（127.0.0.1:9000，bucket media）
- biz 全量（带对象存储）：
  ```bash
  cd /home/jackx/IMs && export IM_MYSQL_PASSWORD=520010 IM_MYSQL_HOST=127.0.0.1 IM_MYSQL_PORT=3306 IM_S3_ENDPOINT=127.0.0.1:9000 IM_S3_REGION=us-east-1 IM_S3_BUCKET=media IM_S3_ACCESS_KEY=minioadmin IM_S3_SECRET_KEY=minioadmin IM_S3_PATH_STYLE=true
  # 先起服务（若未运行）：
  #   setsid nohup ./build/imserver 9191 > /tmp/imserver.log 2>&1 < /dev/null &
  python3 scripts/biz_test.py   # 默认连 127.0.0.1:9191
  ```
- 代码注释一律中文；commit 信息中文；真实用户 id1/2 数据不可动（biz 测试自带精确清理）
- 已知非缺陷：`presence online flip`（时序 flaky，重跑即过）、`send during mysql outage`（本地 mysqld 占 3306 的环境冲突，必 FAIL 可忽略）

---

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `db/migrate_recall.sql` | 创建 | 存量库加 `messages.recalled_ts`（幂等） |
| `db/schema.sql` | 修改 | 新库同步列定义 + status 注释补 3 |
| `src/message_repository.hpp` | 修改 | `kMsgRecalled=3`、`RecallResult`/`RecallInfo`、`recallMessage` 接口 |
| `src/mysql_message_repository.hpp` | 修改 | override 声明 |
| `src/mysql_message_repository.cpp` | 修改 | `recallMessage` 实现；`markRead` 谓词收紧；`hasMediaAccess` 排除撤回行 |
| `src/appserver.hpp` | 修改 | `onRecall`/`messageToJson` 声明 |
| `src/appserver.cpp` | 修改 | 分发表注册 `msg.recall` |
| `src/server_config.hpp` | 修改 | `kRecallWindowSec=120` |
| `src/app_message.cpp` | 修改 | `messageToJson` helper + 四出口替换 + `onRecall` 实现 |
| `src/app_group.cpp` | 修改 | `onGroupHistory` 出口替换 |
| `scripts/biz_test.py` | 修改 | 撤回测试段（10 用例，独立账号自清理） |
| `docs/protocol.md` | 修改 | v3.1：版本变更行 + §5.32 + 状态机说明 |
| `docs/client_api.md` | 修改 | 撤回事件与占位符字段同步 |

---

### 任务 1：迁移 + schema

**文件：**
- 创建：`db/migrate_recall.sql`
- 修改：`db/schema.sql:44-45`（delivered_ts/read_ts 行附近）与 `db/schema.sql:26-30`（status 注释）

- [ ] **步骤 1：创建 `db/migrate_recall.sql`**（沿用 migrate_media.sql 的幂等风格）

```sql
-- 迁移：消息撤回（msg.recall）——messages 增 recalled_ts 列
-- 背景：status 扩展终态 3=RECALLED；recalled_ts 记录撤回时间（审计/排障）。
-- 历史数据 recalled_ts 默认 0，保持回读兼容。
-- 幂等：查询 information_schema.COLUMNS 判定后加列，可重复执行。
-- 用法：MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_recall.sql

SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='messages' AND column_name='recalled_ts');
SET @s := IF(@c = 0,
  'ALTER TABLE messages ADD COLUMN recalled_ts BIGINT NOT NULL DEFAULT 0',
  'SELECT 1');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
```

- [ ] **步骤 2：schema.sql 同步**——在 `read_ts BIGINT NOT NULL DEFAULT 0,`（约 45 行）之后加一行：

```sql
  recalled_ts   BIGINT       NOT NULL DEFAULT 0, -- 撤回时间（msg.recall；0=未撤回）
```

并把表头状态机注释（26-30 行）改为：

```sql
-- status 状态机：
--   0 = OFFLINE_PENDING  目标离线，待补发
--   1 = DELIVERED        已投递（在线投递 或 离线消息已由客户端确认）
--   2 = READ             接收方已读（msg.read）
--   3 = RECALLED         已撤回（终态，msg.recall；历史/离线出口剥离正文）
```

- [ ] **步骤 3：本地库执行迁移并验证**

```bash
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -P 3306 IMS_reactor_db < db/migrate_recall.sql
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -P 3306 IMS_reactor_db -e "SHOW COLUMNS FROM messages LIKE 'recalled_ts';"
# 再次执行验证幂等（无报错）
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -P 3306 IMS_reactor_db < db/migrate_recall.sql
```

预期：SHOW COLUMNS 输出 `recalled_ts | bigint | NO | | 0`；第二次执行无错误。

- [ ] **步骤 4：Commit**

```bash
git add db/migrate_recall.sql db/schema.sql && git commit -m "feat: messages 增 recalled_ts 列（撤回终态 3=RECALLED 迁移）"
```

---

### 任务 2：存储层（recallMessage + markRead/hasMediaAccess 修正）

**文件：**
- 修改：`src/message_repository.hpp`（enum 与接口）
- 修改：`src/mysql_message_repository.hpp`（override 声明）
- 修改：`src/mysql_message_repository.cpp`（实现 + 两处谓词修正）

- [ ] **步骤 1：`message_repository.hpp` 扩展状态与接口**

`enum MessageStatus`（约 25 行）追加一个值；紧随其后新增两个类型；`MessageRepository` 类内（`markRead` 声明之后）新增虚方法：

```cpp
// 消息状态（与 messages 表 status 列一致）
enum MessageStatus {
  kMsgOfflinePending = 0,
  kMsgDelivered = 1,
  kMsgRead = 2,
  kMsgRecalled = 3,  // 撤回终态（msg.recall）：出站序列化剥离正文，仅发送者可撤
};

// 撤回结果分类（recallMessage：affected=0 时回查判定）
enum class RecallResult {
  kOk,        // 撤回成功（状态已迁移）
  kAlready,   // 该消息已被撤回（幂等成功）
  kNotFound,  // 消息不存在
  kForbidden, // 非发送者
  kExpired,   // 超过可撤回时间窗
  kError,     // 参数非法/DB 错误
};

// 撤回成功后供 notify 扇出的行信息（回查所得，chat_type/group_id/to_user 不可变）
struct RecallInfo {
  std::string chatType;  // "single" / "group"
  int64_t groupId = 0;   // 群聊 >0；单聊 0
  int64_t toUser = 0;    // 单聊对端（自己对话框 = 发送者自己）
};
```

类内新增（`virtual void markRead(...)` 之后）：

```cpp
  // 撤回消息：单条 UPDATE 原子校验（存在 message_id + 发送者 from_user +
  // 时间窗 ts >= now-windowSec + 状态迁移 IN(0,1,2)→3 终态），免 TOCTOU。
  // affected=0 回查分类失败原因；kOk 时经 info 回传出站所需行信息
  // （notify 扇出用，含已撤回与已分类两种回查）。info 可为 nullptr。
  virtual RecallResult recallMessage(const std::string& messageId,
                                     int64_t senderId, int64_t nowSec,
                                     int64_t windowSec, RecallInfo* info) = 0;
```

- [ ] **步骤 2：`mysql_message_repository.hpp` 加 override 声明**（与其它方法排列风格一致）：

```cpp
  RecallResult recallMessage(const std::string& messageId, int64_t senderId,
                             int64_t nowSec, int64_t windowSec,
                             RecallInfo* info) override;
```

- [ ] **步骤 3：`mysql_message_repository.cpp` 实现**（放在 `markRead` 实现之后）。messageId 是客户端输入，必须走预处理语句：

```cpp
MessageRepository::RecallResult MySqlMessageRepository::recallMessage(
    const std::string& messageId, int64_t senderId, int64_t nowSec,
    int64_t windowSec, RecallInfo* info) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || messageId.empty() || senderId <= 0 || windowSec <= 0) {
    return RecallResult::kError;
  }
  // 单条 UPDATE 原子校验：存在性 + 权限 + 时间窗 + 状态迁移（免 TOCTOU）
  int errNo = 0;
  MYSQL_STMT* stmt = client_.stmtExec(
      "UPDATE messages SET status = ?, recalled_ts = ? "
      "WHERE message_id = ? AND from_user = ? AND status IN (?, ?, ?) AND ts >= ?",
      {std::to_string(kMsgRecalled), std::to_string(nowSec), messageId,
       std::to_string(senderId), std::to_string(kMsgOfflinePending),
       std::to_string(kMsgDelivered), std::to_string(kMsgRead),
       std::to_string(nowSec - windowSec)},
      &errNo);
  if (!stmt) {
    LOG_E("[mysql] 撤回消息失败 errno=" << errNo);
    return RecallResult::kError;
  }
  const my_ulonglong affected = mysql_stmt_affected_rows(stmt);
  mysql_stmt_close(stmt);
  if (affected != 1) {
    // 失败分类：回查该行（仅失败路径；chatType/groupId/to_user 均不可变，
    // 与 UPDATE 之间无 TOCTOU 风险）
    errNo = 0;
    MYSQL_STMT* sel = client_.stmtExec(
        "SELECT from_user, status, ts FROM messages WHERE message_id = ?",
        {messageId}, &errNo);
    if (!sel) {
      LOG_E("[mysql] 撤回分类查询失败 errno=" << errNo);
      return RecallResult::kError;
    }
    MYSQL_BIND res[3];
    int64_t fromUser = 0, ts = 0;
    int status = 0;
    memset(res, 0, sizeof res);
    res[0].buffer_type = MYSQL_TYPE_LONGLONG;
    res[0].buffer = &fromUser;
    res[1].buffer_type = MYSQL_TYPE_LONG;
    res[1].buffer = &status;
    res[2].buffer_type = MYSQL_TYPE_LONGLONG;
    res[2].buffer = &ts;
    const bool hasRow =
        !mysql_stmt_bind_result(sel, res) && !mysql_stmt_store_result(sel) &&
        mysql_stmt_fetch(sel) == 0;
    mysql_stmt_close(sel);
    if (!hasRow) return RecallResult::kNotFound;
    if (fromUser != senderId) return RecallResult::kForbidden;
    if (status == kMsgRecalled) return RecallResult::kAlready;
    if (ts < nowSec - windowSec) return RecallResult::kExpired;
    return RecallResult::kError;  // 与 UPDATE 条件相符却未命中：并发删除等异常
  }
  // 成功：回查行信息供 notify 扇出（chat_type/group_id/to_user 不可变）
  if (info) {
    errNo = 0;
    MYSQL_STMT* sel = client_.stmtExec(
        "SELECT chat_type, group_id, to_user FROM messages WHERE message_id = ?",
        {messageId}, &errNo);
    if (!sel) {
      LOG_E("[mysql] 撤回信息回查失败 errno=" << errNo);
      info->chatType = "single";  // 回查失败降级：notify 缺失可由占位符兜底
      return RecallResult::kOk;
    }
    MYSQL_BIND res[3];
    char chatBuf[16] = {0};
    unsigned long chatLen = 0;
    int64_t groupId = 0, toUser = 0;
    memset(res, 0, sizeof res);
    res[0].buffer_type = MYSQL_TYPE_STRING;
    res[0].buffer = chatBuf;
    res[0].buffer_length = sizeof chatBuf;
    res[0].length = &chatLen;
    res[1].buffer_type = MYSQL_TYPE_LONGLONG;
    res[1].buffer = &groupId;
    res[2].buffer_type = MYSQL_TYPE_LONGLONG;
    res[2].buffer = &toUser;
    if (!mysql_stmt_bind_result(sel, res) && !mysql_stmt_store_result(sel) &&
        mysql_stmt_fetch(sel) == 0) {
      info->chatType.assign(chatBuf, chatLen);
      info->groupId = groupId;
      info->toUser = toUser;
    } else {
      info->chatType = "single";  // 降级同上
    }
    mysql_stmt_close(sel);
  }
  return RecallResult::kOk;
}
```

- [ ] **步骤 4：`markRead` 谓词修正（防撤回被已读复活）**——`mysql_message_repository.cpp` 约 266 行，把：

```cpp
      "UPDATE messages SET status = ?, read_ts = ? "
      "WHERE to_user = ? AND from_user = ? AND status != ? AND ts <= ? AND group_id = 0",
      {std::to_string(kMsgRead), std::to_string(readTs), std::to_string(toUser),
       std::to_string(fromUser), std::to_string(kMsgRead), std::to_string(readTs)},
```

改为（`status != 2` 会把 RECALLED=3 复活成 2）：

```cpp
      "UPDATE messages SET status = ?, read_ts = ? "
      "WHERE to_user = ? AND from_user = ? AND status IN (?, ?) AND ts <= ? AND group_id = 0",
      {std::to_string(kMsgRead), std::to_string(readTs), std::to_string(toUser),
       std::to_string(fromUser), std::to_string(kMsgOfflinePending),
       std::to_string(kMsgDelivered), std::to_string(readTs)},
```

- [ ] **步骤 5：`hasMediaAccess` 排除撤回行（防图片经 media.url 泄露）**——同文件约 235 行，EXISTS 内 messages 行谓词加 status 过滤，把：

```cpp
      "SELECT EXISTS(SELECT 1 FROM messages WHERE media_id="
      "(SELECT id FROM media WHERE file_id=?) "
      "AND ((group_id=0 AND (from_user=? OR to_user=?)) "
```

改为：

```cpp
      "SELECT EXISTS(SELECT 1 FROM messages WHERE media_id="
      "(SELECT id FROM media WHERE file_id=?) "
      "AND status != " + std::to_string(kMsgRecalled) + " "
      "AND ((group_id=0 AND (from_user=? OR to_user=?)) "
```

（注意相邻行字符串拼接后 SQL 语法完整；参数列表不变。）

- [ ] **步骤 6：构建验证**

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"
```

预期输出：`0`。

- [ ] **步骤 7：单测回归**

```bash
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"
```

预期：`100% tests passed, 0 tests failed out of 13`。

- [ ] **步骤 8：Commit**

```bash
git add src/message_repository.hpp src/mysql_message_repository.hpp src/mysql_message_repository.cpp
git commit -m "feat: recallMessage 原子撤回（status=3 终态）；markRead/hasMediaAccess 修正防复活与泄露"
```

---

### 任务 3：序列化 helper + 四出口

**文件：**
- 修改：`src/appserver.hpp`（声明 `messageToJson`）
- 修改：`src/app_message.cpp`（helper 实现 + `onHistory`/`pushOfflineMessages` 两段替换）
- 修改：`src/app_group.cpp`（`onGroupHistory` 替换）

- [ ] **步骤 1：`appserver.hpp` 声明**（`onRead` 声明附近，private 区）：

```cpp
  // 消息出站序列化（单一出口）：status=RECALLED 时剥离 content/mediaId，
  // 输出 recalled:true 占位符；正常消息与既有字段逐一对齐（行为不变）。
  // groupName 非空时追加 groupName 字段（群历史/群补发用）。
  json messageToJson(const StoredMessage& m, const std::string& groupName = "");
```

（`StoredMessage` 类型已随 `message_repository.hpp` 可见；若不可见则补 include。）

- [ ] **步骤 2：`app_message.cpp` 实现 helper**（放在 `nextMessageId` 之后）：

```cpp
json AppServer::messageToJson(const StoredMessage& m, const std::string& groupName) {
  // 撤回终态：正文/媒体在唯一出口剥离（四出口共用本函数，防单点遗漏）
  const bool recalled = (m.status == kMsgRecalled);
  std::string fromNick;
  userStore_->nickname(m.fromUser, &fromNick);
  std::string fromAccount;
  if (!userStore_->accountOf(m.fromUser, &fromAccount)) fromAccount.clear();
  json j = {
    {"from", fromAccount},
    {"fromNick", fromNick},
    {"content", recalled ? std::string() : m.content},
    {"ts", m.ts},
    {"clientMsgId", m.clientMsgId},
    {"messageId", m.messageId},
    {"chatType", m.chatType},
    {"msgType", m.msgType},
    // 媒体消息透传 fileId（存于 content）；撤回后剥离
    {"mediaId", (!recalled && m.msgType == "image") ? m.content : std::string()},
  };
  if (recalled) j["recalled"] = true;
  if (m.groupId != 0) {
    j["groupId"] = m.groupId;
    if (!groupName.empty()) j["groupName"] = groupName;
  }
  return j;
}
```

- [ ] **步骤 3：替换 `onHistory` 出口**（`app_message.cpp` 约 473-490 行）——循环体改为：

```cpp
  for (const auto& m : stored) {
    arr.push_back(messageToJson(m));
  }
```

- [ ] **步骤 4：替换 `pushOfflineMessages` 单聊段**（约 50-67 行循环体）：

```cpp
  for (const auto& m : stored) {
    arr.push_back(messageToJson(m));
  }
```

- [ ] **步骤 5：替换 `pushOfflineMessages` 群段**（约 78-97 行循环体）——加跳过 + helper：

```cpp
    for (const auto& m : grows) {
      if (m.fromUser == userId) continue;      // 跳过发送者自己的消息（与在线路径一致）
      if (m.status == kMsgRecalled) continue;  // 已撤回不补发（未见过的人不收占位符）
      arr.push_back(messageToJson(m, gname));
      maxTs = std::max(maxTs, m.ts);
      groupRepo_->setWatermark(gid, userId, m.ts);
    }
```

（原 fromNick/fromAccount 查询与 json 构造删除，由 helper 承担；`gname` 已在外层取得。）

- [ ] **步骤 6：替换 `onGroupHistory` 出口**（`app_group.cpp` 约 523-539 行循环体）：

```cpp
  for (const auto& m : stored) {
    arr.push_back(messageToJson(m, gname));
  }
```

- [ ] **步骤 7：构建 + 单测**

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"
```

预期：`0` 与 `100% tests passed ... out of 13`。

- [ ] **步骤 8：Commit**

```bash
git add src/appserver.hpp src/app_message.cpp src/app_group.cpp
git commit -m "refactor: 消息出站序列化收敛 messageToJson（撤回占位符单点剥离，四出口统一）"
```

---

### 任务 4：onRecall + 接线 + 常量

**文件：**
- 修改：`src/server_config.hpp`（常量）
- 修改：`src/appserver.hpp`（onRecall 声明）
- 修改：`src/appserver.cpp:255` 附近（分发注册）
- 修改：`src/app_message.cpp`（onRecall 实现 + 文件头注释行补充）

- [ ] **步骤 1：`server_config.hpp` 常量**（`kMaxOfflineAckIds` 附近）：

```cpp
// 消息撤回时间窗：发送者可在消息落库后 120 秒内撤回（msg.recall）
inline constexpr int64_t kRecallWindowSec = 120;
```

- [ ] **步骤 2：`appserver.hpp` 声明**（`onRead` 声明之后）：

```cpp
  void onRecall(const std::shared_ptr<Session>& session, const json& data);
```

- [ ] **步骤 3：`appserver.cpp` 分发表注册**（`{"msg.read", &AppServer::onRead},` 之后）：

```cpp
    {"msg.recall", &AppServer::onRecall},
```

- [ ] **步骤 4：`app_message.cpp` 实现 `onRecall`**（放在 `onRead` 实现之后）：

```cpp
void AppServer::onRecall(const std::shared_ptr<Session>& session, const json& data) {
  // 消息撤回（msg.recall）：仅发送者、落库后 kRecallWindowSec 内可撤。
  // 原子校验在存储层单条 UPDATE 完成（存在+权限+时间窗+状态，免 TOCTOU）。
  // 成功回执后扇出 msg.recall_notify：单聊=对端全部在线端 + 发送者其他端；
  // 群聊=除发起端外全部在线成员端。重复撤回幂等回 0 且不重复扇出。
  const char* kRes = "msg.recall_res";
  if (!data.contains("messageId") || !data["messageId"].is_string() ||
      data["messageId"].get<std::string>().empty()) {
    session->reply(makeResult(kRes, 400, "参数错误", json::object()));
    return;
  }
  // 限流：与消息发送同桶（撤回频率远低于发送，不设专桶）
  if (!msgLimiter_->allow(session->userId(), kMaxSendPerWindow, kSendWindow)) {
    g_metrics.rateLimited.fetch_add(1, std::memory_order_relaxed);
    session->reply(makeResult(kRes, 429, "操作过于频繁，请稍后再试", json::object()));
    return;
  }
  const std::string messageId = data["messageId"].get<std::string>();
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  MessageRepository::RecallInfo info;
  const auto r =
      msgRepo_->recallMessage(messageId, session->userId(), now, kRecallWindowSec, &info);
  switch (r) {
    case MessageRepository::RecallResult::kOk:
      break;
    case MessageRepository::RecallResult::kAlready:  // 幂等成功（首次已扇出）
      session->reply(makeResult(kRes, 0, "ok", json::object()));
      return;
    case MessageRepository::RecallResult::kNotFound:
      session->reply(makeResult(kRes, 404, "消息不存在", json::object()));
      return;
    case MessageRepository::RecallResult::kForbidden:
      session->reply(makeResult(kRes, 403, "无权撤回该消息", json::object()));
      return;
    case MessageRepository::RecallResult::kExpired:
      session->reply(makeResult(kRes, 410, "超过可撤回时间", json::object()));
      return;
    case MessageRepository::RecallResult::kError:
      session->reply(makeResult(kRes, 500, "服务器内部错误", json::object()));
      return;
  }
  session->reply(makeResult(kRes, 0, "ok", json::object()));
  // notify 扇出：先回执后推送（与 onSend ack-then-deliver 同序）；
  // 回查失败/信息缺失时静默跳过——撤回已生效，同步缺失由占位符兜底
  // （与 msg.read_notify 免 ACK 同先例）
  if (info.chatType.empty()) return;
  std::string fromAccount;
  if (!userStore_->accountOf(session->userId(), &fromAccount)) fromAccount.clear();
  json nd = {{"messageId", messageId}, {"chatType", info.chatType},
             {"from", fromAccount}};
  if (info.chatType == "group") {
    if (info.groupId <= 0) return;
    nd["groupId"] = info.groupId;
    for (const int64_t uid : groupRepo_->listMemberIds(info.groupId)) {
      for (auto& t : onlineMgr_.sessions(uid)) {
        if (t.get() == session.get()) continue;  // 发起端不收（发送者其他端仍收）
        t->reply(makeEnvelopeText("msg.recall_notify", nd));
      }
    }
    return;
  }
  // 单聊：对端全部在线端 + 发送者其他端；自己对话框（to==自己）只发其他端
  if (info.toUser != session->userId()) {
    for (auto& t : onlineMgr_.sessions(info.toUser)) {
      t->reply(makeEnvelopeText("msg.recall_notify", nd));
    }
  }
  for (auto& t : onlineMgr_.sessions(session->userId())) {
    if (t.get() == session.get()) continue;
    t->reply(makeEnvelopeText("msg.recall_notify", nd));
  }
}
```

- [ ] **步骤 5：`app_message.cpp` 文件头注释**（第 20 行 `onHistory / onGroupHistory` 注释行后加）：

```cpp
// - onRecall：消息撤回（发送者+120s 窗内；存储层单条 UPDATE 原子校验；
//   recall_notify 扇出对端/群成员/多端；重复撤回幂等）
```

- [ ] **步骤 6：构建 + 单测**

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"
```

预期：`0` 与 `100% tests passed ... out of 13`。

- [ ] **步骤 7：Commit**

```bash
git add src/server_config.hpp src/appserver.hpp src/appserver.cpp src/app_message.cpp
git commit -m "feat: msg.recall 路由（发送者+120s 原子撤回，recall_notify 多端/群扇出，重复撤回幂等）"
```

---

### 任务 5：biz 集成测试（10 用例）

**文件：**
- 修改：`scripts/biz_test.py`（在群管理段之后、`m1.close()` 清理段之前插入独立撤回段；账号加入清理列表）

**上下文：** biz_test.py 为顺序集成测试：`WS()` 建连、`register(ws,acct,pwd)`、`login(ws,acct,pwd)`、`send(ws,typ,data)`、`send_msg(ws,to,content,cid)`、`env(typ,data)`、`check(name,cond)`、`results/skipped` 全局收集。撤回段用独立账号/独立连接，避免污染前段状态。

- [ ] **步骤 1：在群管理段结束（`m1.close(); m2.close(); m3.close(); m4.close()` 与 `g1.close(); g2.close()` 之后、`a.close(); b.close()` 之前）插入撤回测试段**：

```python
    # ================= 消息撤回（msg.recall）=================
    # 独立账号/连接，避免污染前段会话状态；DB 注入仅用于超窗用例。
    RA, RB, RC = base + "21", base + "22", base + "23"
    r1 = WS()   # 发送者主连接
    r1x = WS()  # 发送者第二连接（多端同步）
    rb = WS()   # 接收者
    rc = WS()   # 离线接收者（用后即关）
    assert register(r1, RA, "pw1234")["data"]["code"] == 0
    assert login(r1, RA, "pw1234")["data"]["code"] == 0
    assert register(r1x, RA, "pw1234")["data"]["code"] == 0
    assert login(r1x, RA, "pw1234")["data"]["code"] == 0
    assert register(rb, RB, "pw1234")["data"]["code"] == 0
    assert login(rb, RB, "pw1234")["data"]["code"] == 0
    assert register(rc, RC, "pw1234")["data"]["code"] == 0  # 注册后不登录（离线）
    # 联系人：RA↔RB、RA↔RC（send 门槛）
    r1.send(env("contact.request", {"account": RB})); json.loads(r1.recv())
    rb.send(env("contact.accept", {"account": RA})); json.loads(rb.recv())
    json.loads(r1.recv()); json.loads(rb.recv())  # 双方 contact 事件
    r1.send(env("contact.request", {"account": RC})); json.loads(r1.recv())
    rc.send(env("contact.accept", {"account": RA})); json.loads(rc.recv())
    json.loads(r1.recv()); json.loads(rc.recv())
    rc.close()  # RC 转离线

    # 1. 单聊撤回全链路：recv → recall → 双端 notify → 历史占位符
    cid1 = "rc1-" + base
    r = send_msg(r1, RB, "撤回我", cid1)
    check("recall send ok", r["data"]["code"] == 0)
    r = json.loads(rb.recv())
    check("recall recv ok", r["type"] == "msg.recv" and r["data"]["clientMsgId"] == cid1)
    rid1 = r["data"]["messageId"]
    r = json.loads(r1x.recv())  # 发送者多端回显
    rid1x = r["data"]["messageId"]
    check("recall echo same id", rid1x == rid1)
    r = send(r1, "msg.recall", {"messageId": rid1})
    check("recall ok", r["type"] == "msg.recall_res" and r["data"]["code"] == 0)
    r = json.loads(rb.recv())
    check("recall notify to peer", r["type"] == "msg.recall_notify"
          and r["data"]["messageId"] == rid1 and r["data"]["chatType"] == "single"
          and r["data"]["from"] == RA)
    r = json.loads(r1x.recv())
    check("recall notify to other device", r["type"] == "msg.recall_notify"
          and r["data"]["messageId"] == rid1)
    r = send(rb, "chat.history", {"to": RA, "limit": 10})
    msgs = r["data"]["data"]["msgs"]
    hit = [m for m in msgs if m["messageId"] == rid1]
    check("recall history placeholder", len(hit) == 1 and hit[0].get("recalled") is True
          and hit[0]["content"] == "" and hit[0]["mediaId"] == "")

    # 2. 群聊撤回：群发 → 发送者撤回 → 成员 notify + 群历史占位符
    r = send(r1, "group.create", {"name": "撤回群"})
    rgid = r["data"]["data"]["groupId"]
    r = send(r1, "group.invite", {"groupId": rgid, "account": RB})
    json.loads(r1.recv())  # member_changed 事件
    r = send_msg(r1, rgid, "群撤回我", "rcg-" + base, chat_type="group")
    check("recall group send ok", r["data"]["code"] == 0)
    r = json.loads(rb.recv())
    rgid_msg = r["data"]["messageId"]
    json.loads(r1x.recv())  # 群回显
    r = send(r1, "msg.recall", {"messageId": rgid_msg})
    check("recall group ok", r["data"]["code"] == 0)
    r = json.loads(rb.recv())
    check("recall group notify", r["type"] == "msg.recall_notify"
          and r["data"]["groupId"] == rgid and r["data"]["chatType"] == "group")
    r = send(rb, "group.history", {"groupId": rgid, "limit": 10})
    ghits = [m for m in r["data"]["data"]["msgs"] if m["messageId"] == rgid_msg]
    check("recall group history placeholder", len(ghits) == 1
          and ghits[0].get("recalled") is True and ghits[0]["content"] == "")

    # 3. 非发送者撤回 403
    r = send_msg(r1, RB, "别撤我的", "rc2-" + base)
    r = json.loads(rb.recv())
    rid2 = r["data"]["messageId"]
    json.loads(r1x.recv())
    r = send(rb, "msg.recall", {"messageId": rid2})
    check("recall non-sender 403", r["data"]["code"] == 403)

    # 4. 不存在 404
    r = send(r1, "msg.recall", {"messageId": "9999999999-999999"})
    check("recall not found 404", r["data"]["code"] == 404)

    # 5. 参数错误 400
    r = send(r1, "msg.recall", {})
    check("recall bad params 400", r["data"]["code"] == 400)

    # 6. 重复撤回幂等 0（对 rid2 发送者再撤一次）
    r = send(r1, "msg.recall", {"messageId": rid2})
    check("recall first ok", r["data"]["code"] == 0)
    json.loads(rb.recv()); json.loads(r1x.recv())  # 消费首次 notify
    r = send(r1, "msg.recall", {"messageId": rid2})
    check("recall twice idempotent 0", r["data"]["code"] == 0)

    # 7. 离线 pending 撤回：RC 不上线则永不收到该消息
    r = send_msg(r1, RC, "离线撤回", "rc3-" + base)
    check("recall offline send ok", r["data"]["code"] == 0)
    r = send(r1, "msg.recall", {"messageId": r["data"]["data"].get("messageId", "")}) \
        if False else None  # messageId 需从回执取：msg.ack 不带，改从 DB 查
```

（**实现注意**：`msg.ack` 不带 messageId。用例 7 的 messageId 用 DB 查询取得：

```python
    import subprocess
    out = subprocess.run(["bash", "-c",
        "MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s IMS_reactor_db -N "
        "-e \"SELECT message_id FROM messages WHERE client_msg_id='rc3-%s';\""
        % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, base)],
        capture_output=True, text=True).stdout.strip()
```

其中 `MYSQL_PWD_ENV`/`MYSQL_PORT_ENV` 为文件内既有全局变量。拿到 `out` 后 `r = send(r1, "msg.recall", {"messageId": out})` 断言 0；然后 RC 登录验证 `msg.offline` 不含该消息、`chat.history` 含占位符：）

```python
    r = send(r1, "msg.recall", {"messageId": out})
    check("recall offline pending ok", r["data"]["code"] == 0)
    r3 = WS()
    assert login(r3, RC, "pw1234")["data"]["code"] == 0
    got_offline = None
    try:
        r3.s.settimeout(2)
        got_offline = json.loads(r3.recv())
    except Exception:
        got_offline = None
    r3.s.settimeout(5)
    if got_offline is not None and got_offline["type"] == "msg.offline":
        mids = [m["messageId"] for m in got_offline["data"]["msgs"]]
        check("recall suppresses offline", out not in mids)
    else:
        check("recall suppresses offline", True)  # 无 offline 包 = 也通过
    r = send(r3, "chat.history", {"to": RA, "limit": 10})
    phits = [m for m in r["data"]["data"]["msgs"] if m["messageId"] == out]
    check("recall offline history placeholder", len(phits) == 1
          and phits[0].get("recalled") is True)

    # 8. 撤回后 markRead 不复活（回归：markRead 谓词 status IN(0,1)）
    #    （用例 1 已撤回 rid1；此处 RB 对 RA 全量已读后再查历史）
    r = send(rb, "msg.read", {"from": RA})
    json.loads(rb.recv())
    r = send(rb, "chat.history", {"to": RA, "limit": 10})
    hit2 = [m for m in r["data"]["data"]["msgs"] if m["messageId"] == rid1]
    check("recall survives markRead", len(hit2) == 1 and hit2[0].get("recalled") is True
          and hit2[0]["content"] == "")

    # 9. 超窗 410（DB 把 rid2 的 ts 改老后撤回）
    #    注意：rid2 在用例 6 已撤回，这里改用新消息
    r = send_msg(r1, RB, "超窗撤回", "rc4-" + base)
    r = json.loads(rb.recv())
    rid4 = r["data"]["messageId"]
    json.loads(r1x.recv())
    subprocess.run(["bash", "-c",
        "MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s IMS_reactor_db -e "
        "\"UPDATE messages SET ts = ts - 3600 WHERE message_id='%s';\""
        % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, rid4)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = send(r1, "msg.recall", {"messageId": rid4})
    check("recall expired 410", r["data"]["code"] == 410)

    # 10. 撤回图片后 media.url 403（对象存储门禁内）
    if medias_enabled:
        png_r = base64.b64decode(PNG_1X1_B64)
        r = send(r1, "media.presign", {"fileName": "rc.png", "mimeType": "image/png",
                                       "size": len(png_r), "kind": "image"})
        r_up = media_data(r).get("uploadUrl", "")
        r_fid = media_data(r).get("fileId", "")
        check("recall image presign ok", bool(r_up) and put(r_up, png_r) in (200, 204))
        r = send(r1, "media.commit", {"fileId": r_fid, "size": len(png_r),
                                      "width": 1, "height": 1})
        check("recall image commit ok", r["data"]["code"] == 0)
        r = send_msg(r1, RB, "", "rc-img-" + base, msg_type="image", media_id=r_fid)
        check("recall image send ok", r["data"]["code"] == 0)
        r = json.loads(rb.recv())
        rid_img = r["data"]["messageId"]
        json.loads(r1x.recv())
        r = send(r1, "msg.recall", {"messageId": rid_img})
        check("recall image ok", r["data"]["code"] == 0)
        json.loads(rb.recv()); json.loads(r1x.recv())  # notify
        r = send(rb, "media.url", {"fileId": r_fid})
        ent = media_data(r).get("urls", {}).get(r_fid, {})
        check("recall image url 403", ent.get("code") == 403)

    # 撤回群清理（群表按 owner 删除走统一清理段，此处仅关连接）
    r1.close(); r1x.close(); rb.close(); r3.close()
```

同时把 `RA, RB, RC` 追加进文件尾部清理列表 `used = [...]`（与 `G1, G2` 同行追加 `+ [RA, RB, RC]`）。

- [ ] **步骤 2：重启服务并全量跑 biz**

```bash
pkill -x imserver; sleep 1
cd /home/jackx/IMs && export IM_MYSQL_PASSWORD=520010 IM_MYSQL_HOST=127.0.0.1 IM_MYSQL_PORT=3306 IM_S3_ENDPOINT=127.0.0.1:9000 IM_S3_REGION=us-east-1 IM_S3_BUCKET=media IM_S3_ACCESS_KEY=minioadmin IM_S3_SECRET_KEY=minioadmin IM_S3_PATH_STYLE=true
setsid nohup ./build/imserver 9191 > /tmp/imserver.log 2>&1 < /dev/null & sleep 2
python3 scripts/biz_test.py 2>&1 | tail -5
```

预期：`~289 tests, 0 failures`（279 基线 + 10 撤回段，media 门禁开启时含图片用例）；`send during mysql outage` 环境性 FAIL 可接受（见环境必读）。若撤回用例失败：先看 `/tmp/imserver.log`，按日志定位（勿绕过断言）。

- [ ] **步骤 3：DB 无残留验证**

```bash
MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -P 3306 IMS_reactor_db -N -e \
  "SELECT COUNT(*) FROM users; SELECT COUNT(*) FROM messages;"
```

预期：users=2（真实用户）；messages 仅真实用户相关行。

- [ ] **步骤 4：Commit**

```bash
git add scripts/biz_test.py
git commit -m "test: msg.recall 十用例（全链路/群/越权/404/超窗/幂等/离线抑制/media隔离/已读不复活/多端）"
```

---

### 任务 6：文档（protocol v3.1 + client_api）

**文件：**
- 修改：`docs/protocol.md`（版本行 + §5.32 + 状态机）
- 修改：`docs/client_api.md`（同步）

- [ ] **步骤 1：protocol.md 版本行**——标题 `# IM 协议文档（v3.0）` 改 `（v3.1）`，版本变更列表加：

```markdown
- v3.1 **消息撤回（msg.recall）**：发送者可在消息落库后 120 秒内撤回（单聊/群聊，仅发送者）；服务端原子状态迁移（`messages.status=3 RECALLED` 终态），历史/离线出口统一剥离正文返回 `recalled:true` 占位符；已撤回图片经 `media.url` 拒绝（403）。多端/对端经免 ACK 的 `msg.recall_notify` 同步（丢失靠重登补发与历史拉取兜底）。见 §5.32
```

- [ ] **步骤 2：protocol.md 新增 §5.32**（在 §5.31 之后，沿用既有事件文档格式）——包含：
  - `msg.recall` 请求/`msg.recall_res` 回执与错误码表（0 成功含幂等/400/403 非发送者/404/410 超窗/429 限流与发送同桶/500）
  - `msg.recall_notify` 数据字段（messageId/chatType/groupId?/from）与扇出规则、免 ACK 兜底说明
  - 占位符字段说明（`recalled:true`、`content:""`、`mediaId:""`；单聊 offline 不再补发已撤回 pending；群补发跳过已撤回）
  - 消息状态机补 `3 = RECALLED（终态）`
  - 客户端语义：收到 notify 后本地将对应 messageId 气泡替换为占位

- [ ] **步骤 3：client_api.md 同步**——撤回事件小节 + 消息对象 `recalled` 字段说明。

- [ ] **步骤 4：Commit**

```bash
git add docs/protocol.md docs/client_api.md
git commit -m "docs: 协议 v3.1 消息撤回（msg.recall/recall_notify/占位符/状态机 RECALLED）"
```

---

### 任务 7：终验 + 合并

- [ ] **步骤 1：全量验证（全部命令在本机执行并记录输出）**

```bash
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"   # 预期 0
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"           # 预期 13/13
# biz 全量（带对象存储，见任务 5 环境）→ 0 failures（mysql outage 环境性除外）
grep -rn "localhost\|127.0.0.1\|/home/jackx" src/ db/ --include="*.cpp" --include="*.hpp" --include="*.sql" | grep -v "127.0.0.1:9000\|IM_S3_ENDPOINT" # 预期无机器路径入库（仅协议示例类命中可说明）
```

- [ ] **步骤 2：合并 master**（`--no-ff` 保分支拓扑；合并后重跑构建+ctest 确认）

```bash
git checkout master && git merge --no-ff feat/message-recall -m "Merge branch 'feat/message-recall': 消息撤回（msg.recall）P0"
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4 2>&1 | grep -ciE "error|warning"
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build 2>&1 | grep "tests passed"
```

- [ ] **步骤 3：推送触发 CI**

```bash
git push origin master
```

（推送属用户已授权的开工范围；CI 结果由用户在 GitHub 网页确认——仓库私有无 gh CLI。）

- [ ] **步骤 4：更新进度账本** `.superpowers/sdd/progress.md`（recall 任务清单 + CI 待确认状态）。

---

## 自检记录（计划完成后执行）

1. **规格覆盖度**：§4 存储→任务 1/2；§5 协议→任务 4/6；§6 onRecall→任务 4；§7 序列化→任务 3；§8 并发→任务 2（谓词）+任务 5（用例 6/9 回归）；§9 安全→任务 2 步骤 4/5 + 任务 3（单点剥离）+任务 5（用例 10）；§10 测试→任务 5 十用例；§11 文档→任务 6；§12 切分→任务 1-7 一一对应。无遗漏。
2. **占位符扫描**：任务 5 步骤 1 中「实现注意」块是对 messageId 获取方式的强制说明（msg.ack 不带 messageId 是既有协议事实），非 TODO；其余步骤均含完整代码/命令。
3. **类型一致性**：`RecallResult`/`RecallInfo`/`recallMessage(…, RecallInfo* info)` 在任务 2 定义、任务 4 使用一致；`kMsgRecalled` 任务 2 定义、任务 3/4 使用一致；`messageToJson(m, groupName)` 任务 3 定义、任务 3 四出口与任务 5 断言字段一致；`kRecallWindowSec` 任务 4 定义并使用；`listMemberIds` 为 `GroupRepository` 既有 API（app_group.cpp:324 在用）。
