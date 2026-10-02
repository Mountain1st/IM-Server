# 消息撤回（msg.recall）设计

日期：2026-10-02
状态：已批准（用户确认方案 A + 三项澄清裁定）
范围：P0 消息生命周期第一步——仅撤回。`msg.delete`/`msg.edit` 留待后续版本（本设计不设障，也不提前实现）。

## 1. 背景与目标

现有消息链路覆盖 发送（msg.send）→ 送达（msg.recv/offline_ack）→ 已读（msg.read），缺少现代 IM 的撤回能力。本次补齐：

```
发送 → 已送达 → 已读 → 撤回（本次）
```

撤回涉及：权限（仅发送者）、消息状态（终态迁移）、多端同步（notify 扇出）、历史数据（占位符）、并发条件（原子状态迁移 + 幂等），是完整的服务端消息生命周期设计点。

## 2. 裁定记录

| 决策点 | 裁定 | 理由 |
|---|---|---|
| 会话范围 | 单聊 + 群聊 | 群消息单行存储（`to_user=0, group_id=G`），实现成本几乎不变 |
| 历史呈现 | 占位符 | 对话流不跳号；`chat.history`/群历史返回 `recalled:true`，content/mediaId 服务端剥离 |
| 原文留存 | DB 留原文 + 接口剥离 | UPDATE 最小、可审计；配套关闭媒体可达性防图片泄露 |
| 状态表示 | **方案 A**：`status=3 RECALLED` 终态 | 单条 UPDATE 原子校验（存在+权限+时间窗+状态迁移，免 TOCTOU）；既有查询路径无一需要新增防泄露谓词（仅失败分类补一次 SELECT）；否决 B（独立标志列：5 处查询路径每处需记得加谓词，漏一处即泄露）与 C（事件表+JOIN：过度设计） |
| 迁移口径 | **无需修改既有 status 字段类型；仅新增 `recalled_ts` 审计时间列** | status 为 TINYINT 原位扩值（0/1/2→3），表结构零改动；`recalled_ts BIGINT NOT NULL DEFAULT 0` 为唯一新增列（审计/排障，`db/migrate_recall.sql`）——不是"零 schema 迁移"，是实现中收窄过的最小迁移 |
| 命名 | `recalled`（不用 revoked） | recall=消息撤回，revoked 易读作权限/令牌撤销 |
| 迁移 | 加一列 `recalled_ts` | 审计/排障价值，代价一列 |

## 3. 状态机

```
0 PENDING → 1 DELIVERED → 2 READ
     └────────┼────────────┘
              ↓
        3 RECALLED（终态）
```

- 单聊：0/1/2 皆可 → 3（时间窗内）。
- 群聊：落库恒为 1（在线投递/水位线补发，见 `app_group.cpp`），故只有 1 → 3。
- 撤回后 delivered/read 语义失效（终态覆盖），现有按 status 精确匹配的查询天然隔离。

## 4. 存储层

### 4.1 Schema

`messages` 增列（`db/migrate_recall.sql` + `db/schema.sql` 同步）：

```sql
ALTER TABLE messages ADD COLUMN recalled_ts BIGINT NOT NULL DEFAULT 0;
```

### 4.2 新增 `recallMessage`

`MessageRepository` 新增：

```cpp
enum class RecallResult { kOk, kAlready, kNotFound, kForbidden, kExpired, kError };
virtual RecallResult recallMessage(const std::string& messageId,
                                   int64_t senderId, int64_t nowSec,
                                   int64_t windowSec) = 0;
```

SQL（原子校验，单条 UPDATE）：

```sql
UPDATE messages
SET status = 3, recalled_ts = ?
WHERE message_id = ? AND from_user = ?
  AND status IN (0, 1, 2) AND ts >= ?;   -- ? = nowSec - windowSec
```

- `affected_rows = 1` → `kOk`。
- `affected_rows = 0` → 补一次 `SELECT from_user, status, ts FROM messages WHERE message_id=?` 分类：
  行不存在 → `kNotFound`；`from_user != senderId` → `kForbidden`；`status = 3` → `kAlready`（幂等）；`ts < now-window` → `kExpired`；其余 → `kError`。

### 4.3 既有消费点修正（审计结论，8 处）

| 消费点 | 现状 | 动作 |
|---|---|---|
| `markRead`（mysql_message_repository.cpp:266） | `status != 2` 🔴 会把 3 复活成 2（内容复活 bug） | 谓词改 `status IN (0, 1)` |
| `hasMediaAccess`（:235-239） | EXISTS 无 status 过滤 🔴 撤回图片仍可签 GET URL | messages 行谓词加 `AND status != 3` |
| 群补发（app_message.cpp pushOfflineMessages 群段） | `fetchGroupMessages` 无 status 过滤 🔴 从未见过该消息的成员会收到占位符 | 补发循环 `if (m.status == kMsgRecalled) continue;`（在线投递已推进水位线 app_group.cpp:631，跳过安全） |
| `fetchOffline`（:133） | `status = 0` 精确匹配 | 无需改（撤回的 pending 不再补发，天然正确） |
| `markDelivered`（:161） | `status = 0` 精确匹配 | 无需改 |
| `purgeExpiredOffline`（:117） | `status = 0` | 无需改 |
| `purgeAgedHistory`（:99） | 群行或 `status != 0` | 无需改（撤回行随保留期老化，与全部历史一致） |
| `saveMessage` | 不读 status | 无需改 |

## 5. 协议（v3.1，新增 §5.32）

### 5.1 `msg.recall`

请求：`{"type":"msg.recall","data":{"messageId":"<ts-seq>"}}`
回执：`msg.recall_res`，`data={}`。

| code | 语义 |
|---|---|
| 0 | 成功（含重复撤回的幂等成功） |
| 400 | 参数错误（messageId 缺失/非字符串/空串） |
| 403 | 无权撤回（非发送者） |
| 404 | 消息不存在 |
| 410 | 超过可撤回时间 |
| 500 | 服务器内部错误 |

安全说明：404/403 区分暴露消息存在性，但 messageId 为服务端生成的 `ts-seq` 不透明串，不可枚举，接受。

### 5.2 `msg.recall_notify`（免 ACK，多端/对端同步）

```json
{"type":"msg.recall_notify","data":{
  "messageId":"...", "chatType":"single|group",
  "groupId":123,            // 仅群聊
  "from":"<撤回者 account>"
}}
```

无 content（服务端不重复下发已剥离内容）。扇出：

- 单聊：接收方全部在线端 + 发送者其他在线端（发起端不收）。
- 群聊：全部在线成员端，除发起端。
- 丢失兜底：重登补发走占位符（离线/历史出口），与 `msg.read_notify` 同先例，notify 不做 ACK 重试。

## 6. 服务层 `onRecall` 流程

`app_message.cpp` 新增，注册进 `appserver.cpp` 分发表：

1. 参数校验：`messageId` 必须为非空字符串，否则 400。
2. 限流：复用 `msgLimiter_`（与发送同桶 30 条/10s；撤回频率远低于发送，不设专桶）。
3. `recallMessage(...)` 原子 UPDATE → 结果映射 res code（`kAlready` → 0 幂等）。
4. 仅 `kOk` 时：回查该行（chatType/groupId/from_user）→ 构造 `msg.recall_notify` → 扇出（先回 res 再扇出，与 onSend ack-then-deliver 同序；notify 失败不影响 res）。
5. `kAlready` 回 0 但**不重复扇出**（首次已扇）。

常量：`kRecallWindowSec = 120`（server_config.hpp，`inline constexpr`）。

## 7. 序列化统一（单点防泄露）

`app_message.cpp` 新增 helper `messageToJson(const StoredMessage&, const std::string& groupName = "")`：

- `status == 3` → `{"from","fromNick","content":"","ts","clientMsgId","messageId","chatType","msgType","recalled":true,"mediaId":""}`（群聊追加 `groupId`/`groupName`）。
- 其余按现格式（四个出口逐字段对齐现状，行为不变）。

四个出口全部改走 helper：

1. `onHistory`（单聊历史）——撤回行 → 占位符。
2. `onGroupHistory`（群历史）——撤回行 → 占位符。
3. `pushOfflineMessages` 单聊段——`fetchOffline` 已排除，helper 兜底。
4. `pushOfflineMessages` 群段——先跳过 `status==3`（§4.3），helper 兜底。

不新增 `status:"recalled"` 字符串字段：线上格式从未暴露数值 status，布尔 `recalled` 是充分最小集。

## 8. 并发矩阵

| 竞态 | 结果 |
|---|---|
| 双击重复撤回 | 第二条 UPDATE affected=0 → SELECT 见 status=3 → 幂等回 0，不重复扇出 |
| recall vs `offline_ack`（`status=0` 谓词） | ack 先落（0→1）→ 撤回仍可（1→3）；撤回先落（0→3）→ ack 空匹配，均安全 |
| recall vs `markRead`（收紧后 `IN (0,1)`） | 已撤回行不被 markRead 触碰（复活 bug 修复后） |
| recall vs 群在线投递 | 撤回落库前的投递照常送达 → 接收端短窗口内已渲染原文，靠 `msg.recall_notify` 改占位（微信同款语义） |
| recall vs `purgeAgedHistory` | 撤回窗口 2min ≪ 保留期，不相交 |

## 9. 安全分析

- **媒体泄露口**：撤回图片经 `media.url` 重取 → `hasMediaAccess` 排除 status=3 → 403。
- **复活口**：markRead 谓词收紧（§4.3）。
- **内容出口**：四处出口单点 helper 剥离；DB 留原文仅服务端可及（审计）。
- **枚举面**：404/403 可区分存在性，messageId 不透明不可枚举，接受。
- **越权**：仅 `from_user=发送者` 可撤回（SQL 谓词内校验）；群管理员代撤不在本期。

## 10. 测试计划

repo 层不新增单测（SQL 薄，经 biz 全链路覆盖，与 media repository 先例一致）。`scripts/biz_test.py` 新增 ~10 用例（独立 `recall` 测试段，独立计数防 flaky 污染）：

1. 单聊撤回全链路：A→B 在线，A recall → A 其他端/B 全部在线端收 `recall_notify`；B `chat.history` 该行 `recalled:true` 且 `content==""`、`mediaId==""`。
2. 群聊撤回：群发 → 发送者 recall → 在线成员（除发起端）收 notify；成员 `group.history` 占位符。
3. 非发送者撤回 → 403。
4. 不存在 messageId → 404。
5. 超窗 → 410（DB 把该行 ts 改老后 recall）。
6. 重复撤回 → 第二次回 0（幂等），notify 不重复。
7. 离线 pending 撤回：B 离线，A 发送后撤回 → B 上线 `msg.offline` 不含该消息；`chat.history` 含占位符。
8. 撤回图片后 `media.url` → 403。
9. 撤回后 `msg.read` 不复活：recall → B msg.read → history 仍占位（回归 §4.3 bug）。
10. 多端发送者同步：A 双连接，端 1 recall → 端 2 收 notify。

测试注入手段：DB 直改 ts（超窗用例）为既有测试先例允许的手段（精确 file_id/message_id 定位，不动真实用户 id1/2 数据）；清理按本次创建账号/messageId 精确删除。

## 11. 文档

- `docs/protocol.md`：v3.0 → v3.1，版本变更行 + §5.32（msg.recall/recall_res/recall_notify）+ 消息状态机补 3=RECALLED。
- `docs/client_api.md`：同步撤回事件与占位符字段。
- `README.md`：无需改动（无新环境变量/部署面变化）。

## 12. 实施切分（供实现计划引用）

| # | 任务 | 交付 |
|---|---|---|
| T1 | 迁移 + schema | `db/migrate_recall.sql`、`schema.sql` |
| T2 | 存储层 | `recallMessage`、`markRead`/`hasMediaAccess` 修正、`kMsgRecalled` |
| T3 | 序列化 helper + 四出口 | `messageToJson`，占位符语义 |
| T4 | `onRecall` + 接线 + 常量 | 分发注册、`kRecallWindowSec`、notify 扇出 |
| T5 | biz 测试 | §10 十用例 |
| T6 | 文档 | protocol v3.1、client_api |
| T7 | 自检 + 合并 | 构建 0 警告、ctest 13/13、biz 全绿、合并 master |
