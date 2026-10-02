# 可靠投递语义 v1（P0.5）设计规格

> 状态：**待评审**。范围：服务端可靠投递闭环 + `msg.ack` 回显 `messageId` + 注册限流真实 IP。
> 目标仓库：`IM-Server`（HEAD `5bdeb20`）。客户端配合改动见 §8。

## 1. 背景与缺陷（已在代码确认）

| # | 缺陷 | 位置 |
|---|---|---|
| 1 | 群离线水位线**在组装 `msg.offline` 时逐条推进**，早于发送、更早于客户端确认；`msg.offline_ack` 不处理群 → 断线即丢 | `src/app_message.cpp:99`（写水位）、`:108`（发送）、`:433-449`（ack）；在线推进 `src/app_group.cpp:617` |
| 2 | 单聊**在线即 delivered**：仅因存在会话、入队成功就落库 `status=1`，无客户端确认 | `src/app_message.cpp:387-388,416-420` |
| 3 | 群离线游标**仅 ts**（`ts > sinceTs`）：同秒多条可跳过/丢；水位线无 `id` | `src/mysql_message_repository.cpp:379-383`；`src/message_repository.hpp:11-23,112-117` |
| 4 | 注册限流按**裸 peerIp**（Nginx 后=代理 IP）；XFF 仅日志 | `src/app_auth.cpp:34`；`src/session.cpp:314-321,327-328` |

核心原则（roadmap §3.9）：**写入队列不能伪装成对方已收到**；`msg.ack` 仅表示"服务端已接收"。

## 2. 目标 / 非目标

**目标**
- G1 单聊在线确认制：客户端确认前不得标记 delivered；断线未确认则登录补发（at-least-once）。
- G2 群离线确认制：水位线仅在**客户端确认**后推进，绝不在推送/组装时推进。
- G3 群 `(ts,id)` 严格游标：水位线含 `id`，谓词严格序，消除同秒跳过。
- G4 `msg.ack` 回显 `messageId`：发送方立即拿到服务端 ID（撤回即时可用）。
- G5 注册（及登录失败）限流按**真实 IP**：可信代理下取 XFF 解析的 `realIp()`，否则 `peerIp()`。

**非目标**
- 群已读/群未读角标（仍非目标）。
- 精确一次（exactly-once）：保持 **at-least-once + 客户端 `messageId` 去重**。
- 多设备已读游标/跨设备同步语义扩展。

## 3. 投递确认协议（关键决策）

统一一个**交付确认**消息，客户端对每条"已进入本地消息模型"的 `msg.recv`/`msg.offline` 回执：

```json
{ "type": "msg.delivered", "data": { "messageIds": ["<id>", "..."] } }
```

- **推荐新增 `msg.delivered`**（语义清晰）；同时**保留 `msg.offline_ack` 并复用同一处理器**（旧客户端/离线批确认继续可用，等价于 delivered）。二者都触发同一 `applyDeliveryAcks(userId, ids)`。
- 单聊：`applyDeliveryAcks` 对每条 `to_user=me AND status=0` 执行 `0→1`，写 `delivered_ts`（即现有 `markDelivered` 语义）。
- 群：对每条 `messageId` 反查 `(group_id, ts, id)`；若该用户是该群成员，推进成员游标 `cursor = max(cursor, (ts,id))`（见 §5 的推进策略与安全性）。
- 单批上限沿用 `kMaxOfflineAckIds=1000`；回执 `msg.delivered_res`/`offline_ack_res` `{code,msg,data:{}}`。

> **决策点 A**：新增 `msg.delivered`（推荐）还是直接把 `msg.offline_ack` 扩展为通用确认（不新增类型）？默认按"新增 + 复用处理"实现，二者对服务端等价。

> **决策点 B（兼容策略）**：新客户端上线前，旧客户端从不确认在线消息 → 消息长期 `PENDING`，每次登录重复补发。建议：配置开关 `IM_DELIVERY_ACK_REQUIRED`（默认 **off** 的过渡期→稳定后 **on**）。默认方案见 §6。

## 4. `msg.ack` 回显 `messageId`

```json
{ "type": "msg.ack", "data": { "code": 0, "msg": "ok", "clientMsgId": "...", "messageId": "..." } }
```

- 单聊与群聊 `msg.ack` 均新增 `messageId`（服务端生成的全局 ID）。
- 纯**增量字段**：旧客户端忽略；新客户端据此立即写 `serverKey`，撤回入口立即可用（消除"需等历史同步"）。
- 失败 ack（`code!=0`）回空 `messageId`。

## 5. 群在线/离线投递与 `(ts,id)` 游标

**Schema 变更**（迁移，见 §6）：
- `group_members.last_delivered_id BIGINT NOT NULL DEFAULT 0`（与 `last_delivered_ts` 组成复合游标）。
- `StoredMessage` 增加 `id`（`messages.id` 主键）；仓储查询返回 `id`。

**游标推进（仅确认后）**：
- **删除** `app_group.cpp:617` 的在线 `setWatermark` 与 `app_message.cpp:99` 的补发 `setWatermark`。
- 仅 `applyDeliveryAcks` → 群分支推进成员游标：
  `setWatermark(gid, uid, maxTs, maxId)`，其中 `(maxTs,maxId)=max((ts,id))` over 本批已确认且属于该群的成员消息。
- **安全性**：同一连接内 `msg.recv` 按 `(ts,id)` 有序推送、客户端按序批量确认；单 TCP 丢帧会连带后续，故确认天然近似连续，`max` 推进不会越过未确认前缀。多设备各自连接、消息流一致，亦安全。极端乱序下的重复由客户端 `messageId` 去重兜底（at-least-once）。

**离线补发谓词（严格序）**：
```sql
WHERE group_id=? AND ( ts > :sinceTs OR (ts = :sinceTs AND id > :sinceId) )
  [AND ts <= :untilTs]  ORDER BY ts ASC, id ASC LIMIT ?
```
- `fetchGroupMessages(groupId, sinceTs, sinceId, untilTs, limit, asc)` 增加 `sinceId` 参数；`(sinceTs,sinceId)` 初值 `(max(wm,joined-1), 0)`。
- 补发仍**不推进水位**；推进只发生在其后客户端 `msg.delivered`。

**在线群推送**：正常向在线成员 `reply(msg.recv)`；**不再** setWatermark；由成员确认推进。

## 6. 单聊状态机变更

- `onSend` 单聊：**一律以 `kMsgOfflinePending(0)` 落库**（自己对话框仍 `kMsgDelivered`），不再按 `targetOnline` 决定初始状态；随后向在线端推送 `msg.recv`。指标拆分：`messagesPushed`（推送数）与 `messagesDelivered`（确认数）。
- delivered 仅由 `applyDeliveryAcks` 产生。
- 已读 `2` 语义不变（`msg.read`，`status IN (0,1)`）。

**兼容策略（决策点 B）**：
- 过渡期：`IM_DELIVERY_ACK_REQUIRED=off` → 在线单聊**仍**先置 `1`（保留旧行为），仅新增群确认与 ack messageId；新客户端上线并验证后切 `on`，单聊改为确认制。
- 或一次性切换（`on`）并要求客户端同步发布。**推荐分两步**（先 off 上线协议，再 on 收紧），降低两端不同步风险。

## 7. 注册 / 登录限流真实 IP（#4）

- 取键：`const std::string ip = session->realIp().empty() ? session->conn()->peerIp() : session->realIp();`
  （`realIp()` 仅在命中 `IM_TRUSTED_PROXIES` 时由 `ws::firstForwardedIp` 赋值，已做注入白名单校验。）
- 应用点：注册限流（`app_auth.cpp:34`）；`kMaxPerIpConnections` 的连接级上限发生在握手前、尚无 XFF，保持 `peerIp`（文档说明）。
- 更新 `docs/nginx-deploy.md:55`：XFF 在可信代理下参与注册限流。

## 8. 客户端配合改动（IM-Client）

1. `msg.ack` 处理增加 `messageId`：立即写 `serverKey`（撤回即时可用），无需等历史。
2. `msg.recv`/`msg.offline`（单聊与群聊）处理后在本地入库后**回执 `msg.delivered`**（按 `messageId` 批量，≤1000；可与离线批合并发送）。撤回占位也回执（已收到）。
3. 群消息补发可能重复：以 `messageId` 去重（已有 `chat/seen`）。
4. 群未读仍不由服务端渲染（§非目标）。

## 9. 测试与验收

**单元**
- 游标谓词：同秒多条 `(ts,id)` 严格序不漏、不重。
- `applyDeliveryAcks`：单聊 0→1；群游标 max 推进；非成员忽略。
- `msg.ack` 带 `messageId`。

**业务回归（`scripts/biz_test.py` 新增）**
- #1 场景：群在线推送后、客户端确认前断开连接 → 重新登录必须**重新补发**（不丢）。
- #2 场景：单聊在线推送后未确认即断开 → 重登补发。
- #3 场景：同秒 3 条群消息全部可达（含分页边界）。
- `msg.ack.messageId` 存在且可用于撤回。
- #4：可信代理 + XFF 下，注册限流按真实 IP 生效。

**命令**：`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build` → `ctest --test-dir build` → `IM_SERVER_BIN=./build/imserver bash scripts/test_biz.sh`。

## 10. 风险

- 协议/DB 迁移需与服务端既有 v3.2 兼容；`group_members` 加列需幂等迁移（既有 migration 机制）。
- 过渡期开关管理：`off→on` 需客户端全量更新后。
- `max` 推进假设（同连接有序确认）已在 §5 论证；极端乱序由客户端去重兜底。
