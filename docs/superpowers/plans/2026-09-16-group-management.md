# 群聊管理实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 实现群聊管理 5 功能（踢人/转让/解散/改名/公告），解开群主无法退群死结。

**架构：** 复用 v1.9 模型（方案 A）：踢人=群主代发起的退群（`left_ts`），转让=改 `owner_id`，
解散=`dissolved` 标记，改名/公告=改 `groups` 列；无新表；处理器仿 `onGroupInvite/onGroupLeave` 模式。

**技术栈：** C++17、MySQL（`mysqlStmtExec` 参数化）、biz_test.py 回归、ctest 单测。

**规格：** `docs/superpowers/specs/2026-09-16-group-management-design.md`

---

## 文件结构

- 修改：`db/schema.sql:69-75` — `groups` 加 4 列（announcement/announce_ts/dissolved/dissolved_ts）
- 修改：`src/group_repository.hpp:29-30` — `getGroup` 加 3 个缺省输出参数；新增 `setOwner/dissolveGroup/renameGroup/setAnnouncement`
- 修改：`src/group_repository.cpp` — 上述 5 方法实现（仿 `getGroup:60-90` 的 bind 写法）
- 修改：`src/app_group.cpp` — 匿名区加 `kMaxAnnouncementBytes=1024`；新增 5 处理器；`onGroupSend` 加 dissolved 检查（必须在 `isMember` 之前）；`onGroupInvite/onGroupMembers` 加 dissolved 检查；`onGroupList` 过滤已解散 + 下发公告字段
- 修改：`src/appserver.hpp:84-90` — 5 个 `onGroup*` 声明
- 修改：`src/appserver.cpp:278-300` — 5 个分发分支
- 修改：`scripts/biz_test.py:693+` — v1.9 群区追加管理用例（目标总数 150+）
- 修改：`docs/protocol.md` + `docs/client_api.md`（+ `docs/docs/` 镜像同步）

---

### 任务 1：DB schema（groups 加 4 列）

**文件：**
- 修改：`db/schema.sql:69-75`

- [ ] **步骤 1：修改 schema**

```sql
CREATE TABLE IF NOT EXISTS `groups` (
  group_id    BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name        VARCHAR(32)  NOT NULL,
  owner_id    BIGINT UNSIGNED NOT NULL,
  created_ts  BIGINT       NOT NULL,
  announcement TEXT        NOT NULL,             -- 群公告正文（单条最新，空=无公告）
  announce_ts  BIGINT       NOT NULL DEFAULT 0,  -- 公告发布时间
  dissolved    TINYINT      NOT NULL DEFAULT 0,  -- 0=正常，1=已解散
  dissolved_ts BIGINT       NOT NULL DEFAULT 0,  -- 解散时间
  KEY idx_owner (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

- [ ] **步骤 2：验证导入成功**

运行：`MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/schema.sql && MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db -e "DESC \`groups\`"`
预期：输出含 announcement/announce_ts/dissolved/dissolved_ts 四列，无报错（`IF NOT EXISTS` 幂等，老库需手动 `ALTER TABLE` 补列，语句见步骤 1 注释）

- [ ] **步骤 3：Commit**

```bash
git add db/schema.sql
git commit -m "feat: groups表加公告/解散四列"
```

---

### 任务 2：GroupRepository（getGroup 扩展 + 4 新方法）

**文件：**
- 修改：`src/group_repository.hpp:29-30`
- 修改：`src/group_repository.cpp`（仿 `getGroup` 的 `mysqlStmtExec` + bind 写法）

- [ ] **步骤 1：改 hpp（`getGroup` 加缺省参数，老 6 处调用零改动）**

```cpp
// 查询群信息，找到返回 true（写出 name/ownerId；后三个可选，传 nullptr 则不写出）。
bool getGroup(int64_t groupId, std::string* name, int64_t* ownerId,
              std::string* announcement = nullptr, int64_t* announceTs = nullptr,
              bool* dissolved = nullptr);
// 转让群主 / 解散 / 改名 / 发布公告（空 text=清空）。成功返回 true。
bool setOwner(int64_t groupId, int64_t newOwnerId);
bool dissolveGroup(int64_t groupId, int64_t now);
bool renameGroup(int64_t groupId, const std::string& name);
bool setAnnouncement(int64_t groupId, const std::string& text, int64_t now);
```

- [ ] **步骤 2：实现（SELECT 加三列，UPDATE 四条仿 `addMember` 的 UPDATE 写法）**

```cpp
// getGroup 的 SQL 改为：
// "SELECT name, owner_id, announcement, announce_ts, dissolved FROM `groups` WHERE group_id = ? LIMIT 1"
// bind 结果 5 列：res[2] STRING announcement（TEXT 用动态缓冲：先 length 查询再取，或复用 64KB 栈缓冲——
// 公告上限 1024 字节，用 char buf[2048] 足够）；res[3] LONGLONG announceTs；res[4] TINY dissolved→bool。
// setOwner: "UPDATE `groups` SET owner_id = ? WHERE group_id = ?"
// dissolveGroup: "UPDATE `groups` SET dissolved = 1, dissolved_ts = ? WHERE group_id = ? AND dissolved = 0"
//   （返回 true 要求 affected_rows==1，重复解散返回 false→上层回 404 群已解散）
// renameGroup: "UPDATE `groups` SET name = ? WHERE group_id = ? AND dissolved = 0"
// setAnnouncement: "UPDATE `groups` SET announcement = ?, announce_ts = ? WHERE group_id = ? AND dissolved = 0"
```

注意：`members_` 缓存不受影响（成员关系未变），四个 UPDATE 无需碰缓存。

- [ ] **步骤 3：构建验证**

运行：`cmake --build build -j4 2>&1 | grep -E 'error|Built target imserver'`
预期：无 error，`Built target imserver`

- [ ] **步骤 4：Commit**

```bash
git add src/group_repository.hpp src/group_repository.cpp
git commit -m "feat: 群仓库加转让/解散/改名/公告方法"
```

---

### 任务 3：踢人 + 转让（红-绿）

**文件：**
- 修改：`src/appserver.hpp`（`onGroupLeave` 声明后加 2 行）
- 修改：`src/appserver.cpp`（`group.leave` 分支后加 2 分支）
- 修改：`src/app_group.cpp`（仿 `onGroupInvite:32-97` 的校验链 + `onGroupLeave:124-132` 的推送）

- [ ] **步骤 1：红探针（写 /tmp/kick_probe.py，用 WS 建群拉人踢人，断言 kick_res code==0）**

```python
# 流程：a建群→加b为联系人→invite b→b用group.kick踢a（b非群主，期望403先验证权限拒绝）；
# a踢b期望code==0；b再发群消息期望403。当前无group.kick分发，服务端回400未知指令→断言失败=红灯。
```

运行探针，预期：`kick_res` 非 0（红灯确认无此指令）

- [ ] **步骤 2：实现 `onGroupKick`（踢人=群主代发起的退群，复用 `removeMember`，缓存自动同步）**

```cpp
void AppServer::onGroupKick(const std::shared_ptr<Session>& session, const json& data) {
  int64_t groupId = 0;
  if (data.contains("groupId") && data["groupId"].is_number_integer()) {
    groupId = data["groupId"].get<int64_t>();
  } else {
    session->reply(makeResult("group.kick_res", 400, "参数错误", json::object()));
    return;
  }
  std::string name;
  int64_t ownerId = 0;
  bool dissolved = false;
  if (!groupRepo_->getGroup(groupId, &name, &ownerId, nullptr, nullptr, &dissolved)) {
    session->reply(makeResult("group.kick_res", 404, "群不存在", json::object()));
    return;
  }
  if (dissolved) {
    session->reply(makeResult("group.kick_res", 404, "群已解散", json::object()));
    return;
  }
  if (ownerId != session->userId()) {
    session->reply(makeResult("group.kick_res", 403, "仅群主可操作", json::object()));
    return;
  }
  const std::string account = data.value("account", "");
  int64_t target = 0;
  std::string nick;
  const int frc = userStore_->lookupAccount(account, &target, &nick);
  if (frc == 2) {
    session->reply(makeResult("group.kick_res", 500, "服务器内部错误", json::object()));
    return;
  }
  if (frc != 0) {
    session->reply(makeResult("group.kick_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (target == ownerId) {
    session->reply(makeResult("group.kick_res", 409, "不能踢群主", json::object()));
    return;
  }
  if (!groupRepo_->isMember(groupId, target)) {
    session->reply(makeResult("group.kick_res", 409, "目标非群成员", json::object()));
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  groupRepo_->removeMember(groupId, target, now);  // 同退群：left_ts=now，窗口保留，可重拉
  session->reply(makeResult("group.kick_res", 0, "ok", json::object()));
  // 被踢者（在线）收 group.removed，其余在线成员收 member_changed(kick)
  json removed = {{"groupId", groupId}, {"name", name}};
  for (auto& t : onlineMgr_.sessions(target)) {
    t->reply(makeEnvelopeText("group.removed", removed));
  }
  std::string targetAccount;
  userStore_->accountOf(target, &targetAccount);
  json changed = {{"groupId", groupId}, {"change", "kick"}, {"account", targetAccount}};
  for (const int64_t uid : groupRepo_->listMemberIds(groupId)) {
    for (auto& t : onlineMgr_.sessions(uid)) {
      t->reply(makeEnvelopeText("group.member_changed", changed));
    }
  }
}
```

`onGroupTransfer` 同构：校验（仅群主；目标存在；目标是成员否则 409；目标不能是自己——转给自己无意义，回 409）→
`groupRepo_->setOwner(groupId, target)` → 回 `group.transfer_res{0}` →
在线成员推 `group.owner_changed{groupId, account:新群主账号}`（用 `accountOf(target)` 取账号）。
分发（`src/appserver.cpp` group.leave 分支后）：
```cpp
if (type == "group.kick") { onGroupKick(session, data); return; }
if (type == "group.transfer") { onGroupTransfer(session, data); return; }
```

- [ ] **步骤 3：构建 + 探针变绿**

运行：`cmake --build build -j4` 后重启服务跑探针
预期：kick_res code==0、removed/kick 事件收到、被踢者群发 403、重拉后只见新窗口

- [ ] **步骤 4：Commit**

```bash
git add src/app_group.cpp src/appserver.hpp src/appserver.cpp
git commit -m "feat: 踢人+转让群主"
```

---

### 任务 4：解散 + dissolved 防线（红-绿）

**文件：**
- 修改：`src/app_group.cpp`（`onGroupDissolve` + 4 处 dissolved 检查 + `onGroupList` 过滤/增字段）
- 修改：`src/appserver.hpp` / `src/appserver.cpp`（声明 + 分发）

- [ ] **步骤 1：红探针（/tmp/dissolve_probe.py：建群→解散→断言 list 不再列出且群发 404）**

预期：红灯（无 `group.dissolve` 指令；且解散前群发成功证明基线正常）

- [ ] **步骤 2：实现 `onGroupDissolve` + 5 处防线**

```cpp
void AppServer::onGroupDissolve(const std::shared_ptr<Session>& session, const json& data) {
  // groupId 整数校验（400）→ getGroup（含 dissolved）→ 不存在 404 → 已解散 404 群已解散 →
  // 非群主 403 仅群主可操作 → dissolveGroup（false 则 404 群已解散）→ 回 dissolve_res{0} →
  // 在线成员（解散前 listMemberIds）推 group.dissolved{groupId}
}
```

防线（每处都是 getGroup 后加 3 行，`dissolved` 为 true 即按规格回绝）：
1. `onGroupSend:232` 后：`ack(404, "群已解散")`（必须在 `isMember:255` 检查之前——成员缓存仍含解散群成员）
2. `onGroupInvite:42` 后：`404 群已解散`
3. `onGroupMembers:158` 前：先 getGroup 查 dissolved（`404 群已解散`），再 isMember（403）
4. `onGroupList:136-146`：循环内 getGroup 取 dissolved，`if (dissolved) continue`；条目加 `{"announcement", ...}, {"announceTs", ...}`（用 getGroup 新输出参数）
5. `onGroupHistory`：不加检查（规格：曾成员仍可查窗口；memberPeriod 天然豁免）
6. `onGroupLeave`：不加检查（解散后退群幂等：成员行 left_ts 已有则 removeMember 覆盖写同一值，上层恒回 ok）

分发：`if (type == "group.dissolve") { onGroupDissolve(session, data); return; }`

- [ ] **步骤 3：构建 + 探针变绿**

预期：dissolve_res 0、dissolved 事件、list 过滤、群发/invite/members 404、history 仍可查

- [ ] **步骤 4：Commit**

```bash
git add src/app_group.cpp src/appserver.hpp src/appserver.cpp
git commit -m "feat: 解散群+dissolved防线"
```

---

### 任务 5：改名 + 公告（红-绿）

**文件：**
- 修改：`src/app_group.cpp`（匿名区常量 + 2 处理器）
- 修改：`src/appserver.hpp` / `src/appserver.cpp`（声明 + 分发）

- [ ] **步骤 1：红探针（/tmp/rename_probe.py：rename/announce 断言 code==0 且 list 下发新值）**

预期：红灯（无指令）

- [ ] **步骤 2：实现（改名复用建群规则，公告空=清空）**

```cpp
namespace {
constexpr size_t kMaxGroupMembers = 50;      // 群人数上限（含群主）
constexpr size_t kMaxGroupNameLen = 32;      // 群名长度上限
constexpr size_t kMaxAnnouncementBytes = 1024;  // 群公告长度上限
}  // namespace

void AppServer::onGroupRename(const std::shared_ptr<Session>& session, const json& data) {
  // groupId 整数校验（400）→ name 空或 >32（400 参数错误）→ getGroup → 不存在 404 →
  // 已解散 404 → 非群主 403 → renameGroup（false 则 404）→ 回 rename_res{0} →
  // 在线成员推 group.renamed{groupId, name}
}

void AppServer::onGroupAnnounce(const std::shared_ptr<Session>& session, const json& data) {
  // groupId 整数校验（400）→ content = data.value("content", "") →
  // content.size() > 1024（413 消息内容超长）→ getGroup → 不存在 404 → 已解散 404 →
  // 非群主 403 → setAnnouncement(groupId, content, now)（空=清空，announce_ts 照写）→
  // 回 announce_res{0} → 在线成员推 group.announced{groupId}（正文走 list 拉取，不扇出大包）
}
```

分发：
```cpp
if (type == "group.rename") { onGroupRename(session, data); return; }
if (type == "group.announce") { onGroupAnnounce(session, data); return; }
```

- [ ] **步骤 3：构建 + 探针变绿**

预期：rename/announce 0、renamed/announced 事件、list 含新名与公告正文、超限 413、清空后 list announcement 为空

- [ ] **步骤 4：Commit**

```bash
git add src/app_group.cpp src/appserver.hpp src/appserver.cpp
git commit -m "feat: 改群名+群公告"
```

---

### 任务 6：biz 永久用例（目标总数 150+）

**文件：**
- 修改：`scripts/biz_test.py`（v1.9 群区后追加，新账号段 `base+"5x"` 避免碰撞，沿 `check("...", ...)` 模式）

- [ ] **步骤 1：追加用例（至少 18 个 check，覆盖规格 §7 全矩阵）**

```python
# 踢人：owner踢成员ok + removed/kick事件 + 被踢群发403 + 重拉只见新窗口
# 踢人权限：非群主403、踢群主409、踢非成员409、目标不存在404
# 转让：转成员ok + owner_changed + 原群主可退群 + 新群主可拉人；非群主403、转非成员409
# 解散：dissolve ok + 事件 + list过滤 + 群发/invite/members 404 + history仍可查
# 改名：ok + 事件 + 空/超长400 + 非群主403
# 公告：发布/清空ok + 事件 + list下发 + 超限413 + 非群主403
```

- [ ] **步骤 2：内存模式全绿**

运行：`bash scripts/test_biz.sh > /tmp/biz_mgmt.log 2>&1`
预期：`150+ tests, 0 failures`（尾行打印总数，以实际为准；失败则修代码不改用例期望）

- [ ] **步骤 3：Commit**

```bash
git add scripts/biz_test.py
git commit -m "test: 群管理biz用例（踢/转让/解散/改名/公告）"
```

---

### 任务 7：文档（protocol + client_api + 镜像）

**文件：**
- 修改：`docs/protocol.md`（版本史加 v2.0 条目；§4 指令表加 5 行；§5 事件表加 6 行；§11.2 高/中危"已修复"列表不动）
- 修改：`docs/client_api.md`（群聊章节加 §5.2x 小节：kick/transfer/dissolve/rename/announce 请求响应示例 + 事件说明）
- 同步：`cp docs/protocol.md docs/client_api.md docs/docs/`

- [ ] **步骤 1：改 protocol.md（指令表仿 group.leave 行格式，事件表仿 member_changed 行格式）**
- [ ] **步骤 2：改 client_api.md（仿 §5.18-5.21 的请求/响应/错误码三段式）**
- [ ] **步骤 3：Commit**

```bash
cp docs/protocol.md docs/client_api.md docs/docs/
git add docs/protocol.md docs/client_api.md
git commit -m "docs: v2.0群管理协议文档"
```

---

### 任务 8：终验（双模式 + ctest + 工作区干净）

- [ ] **步骤 1：Redis 模式全绿**

运行：`IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380 bash scripts/test_biz.sh`
预期：0 failures（Redis 未起则先 `redis-server --port 6380 --daemonize yes --save '' --appendonly no`）

- [ ] **步骤 2：ctest 全绿 + 无残留进程**

运行：`ctest --test-dir build 2>&1 | tail -2; pgrep -x imserver || echo "server stopped"`
预期：`100% tests passed`，无 imserver 残留

- [ ] **步骤 3：确认提交完整**

运行：`git status --short | grep -v '^??'; git log --oneline -8`
预期：跟踪文件无未提交改动（`??` 未跟踪项不动），近 8 条为本期 commit

---

## 自检

1. **规格覆盖度：** 踢人→任务3；转让→任务3；解散→任务4；改名→任务5；公告→任务5；
   dissolved 防线（send/invite/members/list/history/leave）→任务4 步骤2；事件 6 个→任务3/4/5；
   错误码矩阵→任务3/4/5 校验链；测试计划→任务6；文档→任务7。§8 非目标无对应任务（正确）。
2. **占位符扫描：** 无"待定/TODO/适当"；所有代码块完整可落；探针脚本给流程不断言细节（由实现者按 biz 模式补全，
   任务6 永久用例为准）。
3. **类型一致性：** `getGroup` 新签名在任务2定义，任务3/4/5 调用一致
   （`getGroup(gid, &name, &ownerId, nullptr, nullptr, &dissolved)`）；
   事件名与规格 §5 一致（removed/owner_changed/dissolved/renamed/announced/member_changed: kick）；
   `kMaxAnnouncementBytes=1024` 与规格 §3 一致。
```

---

**计划已完成并保存到 `docs/superpowers/plans/2026-09-16-group-management.md`。两种执行方式：**

**1. 子代理驱动（推荐）** - 每个任务调度一个新的子代理，任务间进行审查，快速迭代

**2. 内联执行** - 在当前会话中使用 executing-plans 执行任务，批量执行并设有检查点

**选哪种方式？**
