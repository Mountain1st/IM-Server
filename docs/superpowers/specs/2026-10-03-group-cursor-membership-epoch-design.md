# 群投递游标：成员资格纪元（membership epoch）设计规格

> 状态：**待评审**。范围：修正群可靠投递游标在「新成员加入 / 退群重入」下的永久停滞与漏收。
> **作用域：仅 `IM_DELIVERY_ACK_REQUIRED=on`（确认制）**；off 模式（秒级水位线）保持现状，见 §5。
> 目标仓库：`IM-Server`（审计基准 `0f8fcdf`）。本规格**仅**定义状态机语义与测试，不含媒体/CI 改动。
> 关联：`docs/superpowers/specs/2026-10-02-reliable-delivery-design.md`（其 §5 的游标推进/下界语义由本规格修订；
> §3 连续前缀语义不变）。

## 1. 背景与已确认缺陷（含实测复现）

群投递确认制（`IM_DELIVERY_ACK_REQUIRED=on`）下，成员游标 `group_members.last_delivered_(ts,id)`
由 `group.offline_ack` 的**连续可交付前缀**推进。审计发现两个状态机缺陷，均已用真实 WebSocket 复现（on 模式）。

### 1.1 P0-1 退群重入后游标永久停滞（实测）

- 重入 `addMember`/`addMemberCapped` 执行 `ON DUPLICATE KEY UPDATE joined_ts=VALUES(joined_ts), left_ts=0`
  （`src/group_repository.cpp:307-311,338-340`），**不重置** `last_delivered_*`。
- 补发下界 `sinceTs = max(cts, joined-1)`（`src/app_message.cpp:94`），ACK 扫描下界为**纯 cursor**
  （`src/app_group.cpp:563-565`）。二者**不一致**。
- 复现（审计一次性脚本，未入库；输出如下）：B 退群 → A 发 X1/X2 → B 重入 → A 发 Y1/Y2 → B 在线收
  Y1/Y2 并 ACK：
  ```
  cursor AFTER acking Y1,Y2 = 1791007281 352   # 仍停在 M0，未推进
  Y2 (ts,id)                = 1791007305 356
  reconnect: Y1=True Y2=True                    # 反复补发，永不收敛
  ```
- 失败方式：X1/X2 因 `joined-1` 下界被补发排除，但 ACK 扫描从旧 cursor 出发仍要求它们 →
  **连续前缀在 X1 处断裂，Y1/Y2 被反复补发，游标永久停滞**。

### 1.2 P0-2 新成员入群前历史成为永久空洞（实测）

- 复现（审计一次性脚本，未入库；输出如下）：A 先发 PRE0..2（B 未入群）→ B 入群 → A 发 M1 → B ACK M1：
  ```
  B joined; cursor = 0 0
  cursor after acking M1 = 0 0        # 期望推进到 M1，实际卡死
  reconnect: M1_redelivered=True
  ```
- 根因同 1.1：`joined-1` 使 PRE 对 B 可扫描，而 `onGroupSend` 落库时（`src/app_group.cpp:642`，`to_user=0`）
  B 尚未入群、不在 `listMemberIds` 中（`:665`）→ B 从未收到 PRE，却必须 ACK 才能推进游标。
- 结论：**加入任何已有消息历史的群（即使仅 1 秒前），游标即卡死在起点。**

### 1.3 权威文档内部矛盾

旧 spec `2026-10-02-reliable-delivery-design.md`：
- §5「游标推进」称群游标用 `max((ts,id))` 推进且"安全"——与实现的连续前缀语义（`4acb731` 强制修订）**冲突**；
- §5「离线补发谓词」规定下界 `(max(wm,joined-1),0)`——该 `joined-1` 正是导致 1.1/1.2 的 `joined` 时间边界假设。
（旧 spec 已加修订头注，正文已就地标注作废。）

本规格**修订**上述两点：游标推进保持连续前缀；下界改为成员资格边界（见 §2）。

## 2. 不变量（本规格的裁定基准）

**I1 — 游标不得跨成员资格纪元继承。**
一次「成为成员」开启一个新 epoch；旧 epoch 的 `last_delivered_(ts,id)` 不得作为新 epoch 的连续 ACK 起点。
退群结束旧 epoch，重入开启新 epoch，各自有独立投递起点。

**I2 — ACK 连续性只约束「当前纪元内、对该用户可交付」的消息。**
以下为非本纪元可交付消息，**不进入连续前缀、可被游标直接跨过，永不阻塞**：
- 入群前置历史消息（`(ts,id) <= 纪元边界`，见 §3）；
- 自身发送的消息；
- 已撤回（`status=3`）的消息。

**I3 — 成员资格边界以 `(ts,id)` 精确表达，不依赖秒级 `ts`。**
边界 = 该用户入群瞬间，该群已有消息的 `(ts,id)` 最大值 `(bts,bid)`（无消息则 `(0,0)`）。
判定：**`(ts,id) > (bts,bid)` 属本纪元；`(ts,id) <= (bts,bid)` 属前置历史。**
`id` 唯一且自增，故同秒消息按 `id` 排序，**不存在同秒归属歧义**（§3.2）。

## 3. 设计

### 3.1 纪元边界即游标初值（仅 on 模式）

不需要新增列。**在确认制（on）下，每次（重）入群时把该成员的投递游标重置为纪元边界 `(bts,bid)`**：

- 成员为老成员、仅断线重连（未退群）→ **不重置**，游标续用（正确：断线不结束纪元）。
- `addMember` / `addMemberCapped`（邀请重入）→ 捕获边界并重置（**仅当 `config::deliveryAckRequired()`**）。
- `createGroup`（建群者入群）→ 边界为 `(0,0)`（群内无消息）。
- **off 模式不变**：不重置（沿用旧水位线与 `setWatermarkTs`），本规格不触及其行为。

因游标只增，且初值 = 边界，**任意时刻 `cursor >= (bts,bid)`**；前置历史 `< 边界 <= cursor`，
自然被下界排除，无需再单独存 `bts,bid`。

### 3.2 同秒边界裁定（必裁定项）

边界 `(bts,bid)` 由「入群瞬间 `SELECT ts,id FROM messages WHERE group_id=? ORDER BY ts DESC,id DESC LIMIT 1`」
捕获。裁定：

| 消息相对边界的 `(ts,id)` | 归属 | 行为 |
|---|---|---|
| `> (bts,bid)`（含 `ts==bts 且 id>bid`） | 本纪元 | 进入投递/连续前缀，需 ACK |
| `<= (bts,bid)` | 前置历史 | 不投递、不要求 ACK、可跨过 |

- **同秒但 id 更大**（入群后同秒发送）→ 属本纪元，必达。
- **同秒但 id 更小/相等**（入群前同秒发送）→ 前置历史，跳过。
- 并发提交竞态：某消息 `INSERT` 的 `id` 在边界 `SELECT` **之后**分配 → `id>bid` → 归本纪元投递（at-least-once，
  用户此时已是成员，可接受）；在**之前**分配 → `id<=bid` → 归前置历史跳过。两种方向均**不产生永久漏收**
  （凡 `>边界` 者必投递），符合 at-least-once。

> 与旧方案对比：不采用「`joined_ts - 1` 应刚好够用」的时间假设——该假设在秒级 `ts` 下必然产生边界漏洞（1.1/1.2）。

### 3.3 下界统一为游标（仅 on 模式分支）

- **补发**（`src/app_message.cpp:76-108`）`ackRequired==true` 分支：
  - 删除 `sinceTs = max(cts, joined-1)` 与条件 `sinceId`；
  - 改为 `fetchGroupMessages(gid, cts, cid, until, 100, true)`（持久游标为唯一低界）。
  - `ackRequired==false`（off）分支**保持现状**：`sinceTs=max(cts,joined-1)`、`sinceId=-1`、补发后 `setWatermarkTs` 不动。
- **ACK 扫描**（`src/app_group.cpp:561-576`）：保持从 `(cts,cid)` 起（已正确），仅依赖 3.1 的重置保证边界。
- 前置历史自动被排除：因为 `cts = 边界`（新纪元）或更大（既在纪元内）。

### 3.4 `joined_ts` 的去留

- `joined_ts` **仅保留给 `group.history`/`memberPeriod` 的历史窗口**（`src/app_group.cpp:522` 的 `joined-1` 不动，
  属独立读取 API，非投递游标）。
- 投递路径（补发/ACK）**不再引用** `joined`。

## 4. 数据与接口改动

**Schema**：无新列、无迁移。存量成员游标保持原值：
- 因同时把 on 模式补发低界改为纯游标，**修复前卡死的游标在下次登录自愈**（补发补齐到卡点前的所有消息，
  客户端 ACK 后可继续推进）；代价是对「修复前已重入的成员」可能一次性多补发其离开期间的消息（at-least-once，
  客户端按 `messageId` 去重），见 §8。
- 修复后新（重）入群成员的边界重置从源头保证入群前历史不进入投递窗口。

**仓储**（`src/group_repository.cpp/hpp`）：
- 新增私有辅助：`bool boundaryCursor(int64_t groupId, int64_t* bts, int64_t* bid)`（群当前最大 `(ts,id)`，无消息返回 0,0）。
  注意：在 `mutex_` 已持有的临界区内调用时须走**无锁变体**（`boundaryCursorLocked(MySqlClient&,...)`，仿
  `mysql_media_repository.cpp` 的 `queryByFileIdLocked` 先例），避免自死锁。
- `addMember` / `addMemberCapped`：在同一 `mutex_` 临界区内，先取边界，再以 `last_delivered_ts=bts,last_delivered_id=bid`
  写入（`ON DUPLICATE KEY UPDATE` 覆盖 `joined_ts`、`left_ts=0` 及游标）。临界区串行化保证本进程内原子；
  极端崩溃窗口（写入失败）→ 游标停在旧值/0，仅导致**更多重复补发**，不丢（at-least-once）。
  重置**仅当 `config::deliveryAckRequired()`**；off 下不写游标列（保持旧行值）。
- `group_repository.cpp` 需 `#include "server_config.hpp"`（当前未包含）。

**业务层**：
- `src/app_message.cpp` `pushOfflineMessages`：按 §3.3（仅 on 分支改低界；off 分支不动）。
- `src/app_group.cpp` `onGroupOfflineAck`：无需改逻辑，行为由 3.1 保证。

## 5. 非目标 / 出范围（明确不做）

- **off 模式**：本次不改 `onGroupSend` 的 `advanceDeliveredCursor(gid,uid,now,0)`（`src/app_group.cpp:677`）与
  `setWatermarkTs` 语义。off 模式仍为秒级水位线，其同秒边界问题作为**独立后续项**（见 §9）。
- 单聊游标（`messages.status` 状态机）与群 epoch 无关，不在本规格。
- 群已读/未读（`lastReadTs`）、多设备已读同步。
- 历史分页、多实例路由、连接池。

## 6. 测试（必须独立成组）

以下用例加入 `scripts/biz_test.py` 投递段（on 模式门禁内），每条独立 `check`：

| 编号 | 场景 | 期望 |
|---|---|---|
| T1 | 新成员加入已有大量历史（A 预设 N 条 → B 入群 → A 发 M1 → B ACK M1） | 入群前消息不进 ACK 前缀；游标推进到 M1；reconnect 不重复 M1 |
| T2 | 退群 → 群内产生消息 → 重入 | 旧游标不阻塞新 epoch；新纪元消息可正常 ACK 推进 |
| T3 | 重入后在线收 Y1/Y2 → ACK → reconnect | 游标到 Y2；reconnect 不再补发 Y1/Y2 |
| T4 | 入群边界同秒消息（预消息与入群后消息同一秒；用 DB 读到的 `ts` 判定是否命中同秒，命中则断言 `id` 归属，未命中也不 skip） | both 情况下：post 可 ACK、游标推进、pre 不阻塞；同秒时 post 按 `id>bid` 归本纪元（§3.2 裁定） |
| T5 | 自身消息位于 ACK 前缀中 | 无需 ACK，游标跨过（等同 biz_test 投递段 #8，确认 epoch 修复后仍成立） |
| T6 | recalled 消息位于 ACK 前缀中 | 无需客户端收到，游标跨过 |
| T7 | ACK / 断线 / reconnect race | 游标只前进、不回退、不永久停滞 |

补充：**CLI 层**新增 `boundaryCursor` 单测（无消息=0,0；有消息=最大 `(ts,id)`）；
`pushOfflineMessages` 低界为游标的断言。

## 7. 验收命令

```
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build
IM_MYSQL_PASSWORD=... IM_DELIVERY_ACK_REQUIRED=1 python3 scripts/biz_test.py   # on 段全绿
IM_MYSQL_PASSWORD=... python3 scripts/biz_test.py                              # off 回归不退化
```

## 8. 风险

- **存量数据**：未 ACK 过的成员游标保持 0 → 首次登录可能大批补发（at-least-once）。可接受；如需收敛，
  后续可另行运营（如手工将游标置为各群当前最大 `(ts,id)` 以跳过历史）——非本规格范围。
- **边界捕获与并发写入**：见 §3.2，方向安全，不会永久漏收。
- **多实例**：`boundaryCursor` + 写入在同一进程 `mutex_` 内；多实例部署下需 DB 级原子（本规格假设单实例，
  与项目现状一致）。

## 9. 后续独立项（不在本规格实施）

1. off 模式 `advanceDeliveredCursor(...,now,0)` 的 id 分量语义（同秒边界）。
2. 旧 spec `2026-10-02-reliable-delivery-design.md` **§5** 的 `max 推进` 正文同步为连续前缀 + epoch 边界
   （当前已加修订头注，正文同步随实现 E4 一并完成）。
3. P0-3 媒体魔数校验接线（审计报告序）。
