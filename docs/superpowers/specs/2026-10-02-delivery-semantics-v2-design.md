# 投递确认语义 v2（Delivery Semantics v2, P0.5）设计

日期：2026-10-02
状态：已批准（用户确认核心模型 + 补死 3 条约束）
范围：一次性收敛整体审查缺陷 #1（Critical 群补发读即推进）、#2（Important 在线乐观 delivered）、#3（Important 群秒级游标同秒丢失）。#4（X-Forwarded-For）为紧随的独立小修，不在本规格。

## 1. 目标语义

```
落库(status=PENDING)
      ↓
尝试投递（单聊 msg.recv / 群 msg.recv）
      ↓
客户端 ACK
      ↓
服务端才推进「已投递」事实
```

核心：**“服务端已发送” ≠ “客户端已确认收到”**。统一为三分：`发送成功 ≠ 投递成功 ≠ 已读`。

## 2. 现状实证（根因）

| 根因 | 位置 | 事实 |
|---|---|---|
| #1 群补发读即推进 | `app_message.cpp:90-99` | 从 DB 读出即 `setWatermark`，reply 在循环之后（:105）；断连也推进 |
| #1 群 ACK 空转 | `mysql_message_repository.cpp:161` | `markDelivered` 谓词 `WHERE to_user=? AND status=0`；群消息 `to_user=0`，ack 传 `toUser=me` 恒不匹配 |
| #2 单聊乐观 delivered | `app_message.cpp:387-388` | `targetOnline` 判定后立即 `status=kMsgDelivered` 落库，推送在 :416 之后 |
| #3 群游标不一致 | `app_group.cpp:617` vs `app_message.cpp:99` | 在线投递推进 `now`（服务器当前秒），补发推进 `m.ts`——同字段两语义，`now` 可能 > 消息 ts 致同秒消息被跳过 |

## 3. 单聊模型（复用现有列，零 schema 迁移）

状态机不变：`0 PENDING / 1 DELIVERED / 2 READ / 3 RECALLED`，仅**推进时机**改变。

```
在线：save status=0 → msg.recv → msg.offline_ack → status 0→1
离线：save status=0 → （不推）→ 登录补发 msg.offline → msg.offline_ack → status 0→1
```

- `saveMessage` **恒 status=0**（删除 `targetOnline ? 1 : 0`）
- 在线仍立即推 `msg.recv`（status 仍是 0）
- `msg.offline_ack{messageIds[]}` → `markDelivered` 谓词 `to_user=me AND status=0`（现有语句，语义扩展为“在线/离线通用投递确认”）
- **自己对话框（to==self）例外**：无 ACK 来源，落库即 delivered（保持现状）
- **at-least-once**：未 ACK 的在线消息会被 `fetchOffline` 重投；客户端按 `messageId` 去重（协议既有要求）
- 撤回：`0 PENDING → 3 RECALLED`；`fetchOffline` 只取 `status=0`，撤回后不再补发（自然闭环）

## 4. 群聊模型（成员级有序游标）

群消息单行（`to_user=0`），行级 `status` 无法表达“A 已收/B 未收”——**投递事实源只能是 `group_members` 成员级游标**。

`group_members` 增列：

```sql
last_delivered_id BIGINT UNSIGNED NOT NULL DEFAULT 0
```

游标 = **`(last_delivered_ts, last_delivered_id)`**（复用现有 `last_delivered_ts` + 新增 id）。

```
在线：msg.recv → 不推进 cursor → group.offline_ack → cursor 前进
离线：登录 fetch cursor+ → msg.offline → group.offline_ack → cursor 前进
```

### 4.1 约束 ①：`group.offline_ack` = 累计 ACK（cumulative）

请求 `{groupId, ts, messageId}` 语义**必须**定义为：

> 客户端已**连续**收到该 `(ts, messageId)` 及之前、在当前可见窗口内的全部群消息。

**不是**“我收到这一条了”。反例：服务端游标停在 A，客户端乱序只收到 A、C 便 ACK C，把 cursor 推到 C，**B 永久跳过**。故：
- 服务端 ACK 任意位置即认为其**之前**（按 (ts,id) 序）均已确认——服务端**无法**区分“真连续前缀”与“乱序误推”
- 因此“只对连续前缀末端发 ACK”是**客户端强制义务**（协议写明）：客户端须维护 `contiguous`，仅在补齐空洞后才 ACK 更高位置（收到 A→contiguous=A；收到 C 不 ACK；补收到 B→contiguous=C→ACK C）
- 服务端侧仅保证单调只进（约束②），不承担空洞检测

### 4.2 约束 ②：`advanceDeliveredCursor()` 严格按 (ts, id) 单调前进

```
newCursor > oldCursor  ⟺  (newTs > oldTs) || (newTs == oldTs && newId > oldId)
```

- 禁止 `messageId` 字符串比较；禁止 `ts >= oldTs`（会把同秒回退）
- 语义等价 `cursor = max(oldCursor, ackCursor)`（按 `(ts, 内部 id)` 字典序）
- 只进不退 → 重复 ACK 幂等（`max` 不变）
- ACK 校验：`groupId > 0`；当前用户仍是群成员（`left_ts=0`）；`messageId` 存在且属于该群；取其 `(ts, id)` 作为候选游标。错误统一 `404 消息不存在或不属于该群`（不过度泄露细节）

### 4.3 约束 ③：cursor 是账号级（user-level），不是设备级

> 群投递确认是**用户级**而非设备级；任一在线设备确认即推进该用户群投递游标。

- 与单聊 `messages.status` 的账号级 delivered 语义一致，**非缺陷**
- 后果：PC 确认、手机未收 → 手机重连不补该段（与现状一致）
- 文档必须写明，避免 `last_delivered_id` 被误读为“所有端都收到”

## 5. 查询与游标同构（实现硬约束）

`fetchGroupMessages` 增加 `sinceId` 参数（history 调用传 0）：

```sql
WHERE group_id = ?
  AND ( ts > ? OR (ts = ? AND id > ?) )
ORDER BY ts ASC, id ASC
```

排序必须与 cursor 比较完全同构（`(ts ASC, id ASC)` ↔ `(ts, id)` 字典序），否则出现“查询排序 ≠ ACK 游标排序”的隐蔽边界 bug。

## 6. 协议（v3.3）

### 6.1 新增 `group.offline_ack`

```
请求：{ "groupId": 12, "ts": 1720000000, "messageId": "..." }   // ts/messageId 指向连续前缀末端
回执：group.offline_ack_res {code, msg, data:{}}
错误码：400 参数错误（groupId/ts/messageId 缺失或非法）
       403 非群成员
       404 消息不存在或不属于该群
       500 服务器内部错误
幂等：重复 ACK（含回退位置）→ code 0 且游标不变
```

### 6.2 `msg.offline_ack` 语义扩展

从“离线消息确认”扩展为**通用投递确认**（单聊在线/离线通用，`messageIds[]`）。响应 `msg.offline_ack_res` 不变。

### 6.3 群 `msg.recv` 字段不变

已含 `groupId/ts/messageId`，客户端据此构造 `group.offline_ack`。

## 7. 改动点清单

| 位置 | 改动 |
|---|---|
| `db/migrate_delivery_cursor.sql` + `schema.sql` | `group_members` 增 `last_delivered_id`（默认 0；存量行 `last_delivered_ts` 保留原值——旧在线投递可能已把 ts 推到 `now` 超前进位，属演进前既有行为，不追补历史，新逻辑仅从部署后按 ACK 严格推进） |
| `app_message.cpp:388` | 删 `targetOnline?1:0`，单聊恒 status=0（自己对话框除外仍 1） |
| `app_message.cpp:90-99` | 补发走 (ts,id) 游标，**移除循环内 `setWatermark`** |
| `app_group.cpp:617` | **删除**在线投递 `setWatermark`（在线不推进游标） |
| `onOfflineAck` | 语义泛化 + 类型校验（messageIds 数组） |
| **新增 `onGroupOfflineAck`** | 校验（成员/消息归属）+ 反查 (ts,id) + 严格只进推进游标 |
| `fetchGroupMessages` | 增 `sinceId` 参数；SQL 改 `(ts,id)` 双条件；排序 `ts ASC,id ASC` |
| `GroupRepository` | `getWatermark`/`setWatermark` → `getDeliveredCursor(gid,uid,&ts,&id)` / `advanceDeliveredCursor(gid,uid,ts,id)`（严格 max 只进） |
| `appserver.cpp` 分发表 | 注册 `group.offline_ack` |
| 协议文档 | v3.3：新指令 + msg.offline_ack 语义扩展 + cursor 三级约束（cumulative/严格单调/账号级） |

## 8. 回归测试（biz 新增段，独立账号）

| 用例 | 断言 |
|---|---|
| 单聊在线未 ACK → 重登补发 | 在线收到 msg.recv；不 ACK；重登 msg.offline 含该消息；ACK 后再重登不含 |
| 单聊 ACK 幂等 | 重复 msg.offline_ack 同 messageIds → code 0，fetchOffline 不再返回 |
| 单聊 ACK 丢失 | 收到不 ACK → 下次补发仍到（at-least-once） |
| 群在线未 ACK → 重登补发 | 群在线收 msg.recv；不 group.offline_ack；重登 msg.offline 含该群消息 |
| 群 cumulative ACK | ACK 连续前缀末端 → cursor 推进；该位置前均已确认 |
| 群 ACK 只进不退/幂等 | 重复 ACK、回退位置 ACK → cursor 不变、code 0 |
| 群 ACK 越权/非法 | 非成员 403；不存在/他群消息 404；参数缺失 400 |
| 同秒消息不丢 | 同秒多条群消息，ACK 前 N 条后重登只补后 M 条（(ts,id) 严格） |
| 撤回 × 补发 | PENDING 消息撤回后不再补发（fetchOffline status=0 排除 3） |
| 已读 × 投递 | markRead 只影响 0/1→2，不影响 PENDING 补发语义 |
| 多端（账号级） | 同账号两端，一端 ACK → 另一端重登不补（账号级语义，文档一致） |

## 9. 已知取舍（记录，不在本期解决）

- 多端落后设备不补已 ACK 段（账号级语义，与单聊一致）
- 群 ACK 客户端须只对连续前缀末端发送（协议义务；服务端无法区分乱序与真前缀）
- at-least-once：客户端必须 messageId 去重
- 在线全确认制为协议破坏性变更（老客户端不发 ACK → 消息持续 PENDING 重投）；客户端尚未联调上线，当前改代价最低
- **不可交付消息不阻塞游标（v3.3 明确）**：群补发跳过当前用户自身发送的消息（`m.fromUser == userId`）与已撤回消息（`status==3`）。此类“当前用户无需接收”的消息**不要求 ACK，也不作为该用户群投递游标的阻塞项**；当客户端 ACK 后续可交付消息时，服务端严格只进推进可**跨过**其前的自身消息与已撤回消息。即：**游标是服务端群消息流中的位置，而 ACK 针对的是“当前用户可交付消息的连续前缀”**。
- **被跨过的是补发通道，不是数据**：自身消息与已撤回消息仍存于 `messages`，`group.history` 照常可取回自身消息（history 按 `(joined-1,0)` 与投递游标无关）。
- **多端自身消息同步不在本规格范围**（YAGNI，保持既有 `continue` 语义）：离线设备不补发“自己其他设备发送的群消息”，属独立的“多端群消息同步策略”议题，P0.5 不引入。

## 10. 影响与后续

本规格一次性消除整体审查 #1/#2/#3，形成统一可靠投递模型。完成后依次处理：
- P0.6：#4 X-Forwarded-For/Nginx 注册限流（独立小修）
- P1：#5 Redis fail-closed 语义裁定
- 剩余 Minor 按账本分诊
