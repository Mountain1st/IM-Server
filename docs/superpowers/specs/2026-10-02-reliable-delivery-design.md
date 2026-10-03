# 可靠投递语义 v1（P0.5）设计规格

> 状态：**部分被修订**。§5 的群游标推进/下界语义已由
> `docs/superpowers/specs/2026-10-03-group-cursor-membership-epoch-design.md` **修订**：
> - §5「游标推进」中的「群游标 `max((ts,id))` 推进 / `setWatermark(gid,uid,maxTs,maxId)`」**作废**，
>   实际语义为**连续可交付前缀**（`4acb731` 强制修订）；
> - §5「离线补发谓词」中的初值 `(max(wm,joined-1),0)` **作废**，改为成员资格纪元边界（新规格 §3）。
> 其余（§3 双 ACK、§4 `msg.ack.messageId`、§6 开关、§7 真实 IP）仍有效。
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

## 3. 投递确认协议（已定：双 ACK）

客户端对每条"已进入本地消息模型"的接收消息回执。**单聊与群聊各用一种 ACK**（群消息 `to_user=0`，无法复用单聊处理器）：

- **单聊**（在线 `msg.recv` 与离线 `msg.offline` 统一）：
  `{ "type": "msg.offline_ack", "data": { "messageIds": ["<id>", "..."] } }` → `msg.offline_ack_res`
- **群聊**（在线群 `msg.recv` 与离线 `msg.offline` 群段统一）：
  `{ "type": "group.offline_ack", "data": { "messageIds": ["<id>", "..."] } }` → `group.offline_ack_res`

- 单批上限沿用 `kMaxOfflineAckIds=1000`；撤回占位也回执（视为"已收到"）。
- 服务端在**阶段 A 与 B 都接收**这两种 ACK；是否据此改变投递事实由全局开关（§6）决定。阶段 A 下 ACK 仅用于幂等/统计，不改变旧语义。
- **不新增 `msg.delivered`**（已被上述两类型取代）。
- **群 ACK 为「连续可交付前缀」语义（强制）**：`group.offline_ack.messageIds[]` 表示客户端已处理的、**从当前 delivery cursor 起点开始的连续可交付消息前缀**（连续前缀末端之前不应有未确认的可交付消息）。服务端反查这些 `messageId` 的 `(ts,id)`，**只能把游标推进到连续前缀的最远位置**，**不得**按本批最大 `(ts,id)` 无条件跃迁。
  - 反例：`A=101 已收 / B=102 未收 / C=103 已收`，客户端 ACK `[101,103]`——服务端只能推进到 101（102 是空洞），**不得**跳到 103，否则 102 永久跳过。
  - 客户端义务：只对本连接内**按序到达的连续前缀末端**发送 ACK（乱序补洞后才 ACK 更高位置）。
  - 服务端实现：对本批 `messageIds` 反查得到候选 `(ts,id)` 集合，取「从旧游标出发、在候选集合与真实消息流中连续可达的最远前缀末端」推进（实现上=对候选按 `(ts,id)` 升序，取**第一个不连续点的前一个**；详见 §5 与 plan T3）。

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
- ~~仅 `applyDeliveryAcks` → 群分支推进成员游标：
  `setWatermark(gid, uid, maxTs, maxId)`，其中 `(maxTs,maxId)=max((ts,id))` over 本批已确认且属于该群的成员消息。~~
  **[已作废]**：群游标为**连续可交付前缀**推进（见新规格 §3）；~~上一行 `max` 推进的"安全性"论证~~不再适用——
  `max` 会越过空洞。实际实现见 `src/app_group.cpp` `onGroupOfflineAck`。

**离线补发谓词（严格序）**：
```sql
WHERE group_id=? AND ( ts > :sinceTs OR (ts = :sinceTs AND id > :sinceId) )
  [AND ts <= :untilTs]  ORDER BY ts ASC, id ASC LIMIT ?
```
- `fetchGroupMessages(groupId, sinceTs, sinceId, untilTs, limit, asc)` 增加 `sinceId` 参数；
  ~~`(sinceTs,sinceId)` 初值 `(max(wm,joined-1), 0)`~~ **[已作废]**：on 模式初值 = 成员资格纪元边界，
  不再引用 `joined-1`（新规格 §3.1/§3.3）。
- 补发仍**不推进水位**；推进只发生在其后客户端 ACK。

**在线群推送**：正常向在线成员 `reply(msg.recv)`；**不再** setWatermark；由成员确认推进。

## 6. 确认制开关（控制单聊 + 群聊，整体切换）

新增配置 `IM_DELIVERY_ACK_REQUIRED`（默认 **off**），**控制整个 delivery semantics**（非仅单聊）：

- **`off`（兼容模式，阶段 A）**：在线推送成功即可视为 delivered——单聊落库即 `status=1`；群按旧语义在推送/补发时推进水位线。两种 ACK 可接收但**不改变投递事实**（仅幂等/统计）。
- **`on`（确认模式，阶段 B）**：发送后保持 `PENDING(0)`；**只有收到客户端 ACK 才推进 delivered fact**——单聊 `status 0→1` + `delivered_ts`；群推进成员投递游标 `(ts,id)`。单聊与群聊**同步切换**。

指标拆分：`messagesPushed`（推送）与 `messagesDelivered`（确认）。已读 `2` 语义不变（`msg.read`，`status IN (0,1)`）。自己对话框恒 `kMsgDelivered`。

> **为何禁止单聊/群聊分灰度**：群游标是**用户级**而非设备级。若只对新客户端开启群确认，而同一账号仍有旧设备不 ACK，新设备 ACK 会把用户级游标推过旧设备未收到的消息 → 旧设备**永久漏收**。故必须：单账号全部在用客户端都具备 ACK 能力后，用**同一开关整体切换**（不引入账号级设备能力协商）。

**发布阶段**：
- **阶段 A**：服务端上线双 ACK 接收 + `off`（维持旧语义）；客户端实现并发送两种 ACK；协议文档 v3.3。
- **阶段 B**：全量客户端更新确认后，`IM_DELIVERY_ACK_REQUIRED=on` 整体切换为确认制。

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
- `applyDeliveryAcks`：单聊 0→1；群游标**连续前缀**推进；非成员忽略。
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
- ~~`max` 推进假设~~ **[已作废]**：群游标为连续可交付前缀（新规格 §3），`max` 论证不再适用。
