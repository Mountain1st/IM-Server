# 可靠投递语义 v1（P0.5）实现计划

> 依赖规格：`docs/superpowers/specs/2026-10-02-reliable-delivery-design.md`。
> 顺序固定：T1→T2→T3→T4→T5→T6→T7；T8 为客户端仓库跟进。

## T1 Schema 与仓储：复合游标 `(ts,id)`
- 迁移：`group_members` 增列 `last_delivered_id BIGINT NOT NULL DEFAULT 0`（幂等，沿用既有 migration 机制）。
- `StoredMessage` 增 `int64_t id`；所有 `SELECT` 返回 `messages.id`。
- `GroupRepository`：`getWatermark(gid,uid)` → 同时取 `(ts,id)`；`setWatermark(gid,uid,ts,id)`。
- `fetchGroupMessages(groupId, sinceTs, sinceId, untilTs, limit, asc)`：谓词严格序（spec §5）。
- 验收：`ctest` 通过；同秒分页不丢。

## T2 协议：`msg.ack.messageId` + `msg.delivered`
- `msg.ack`（单/群）`data` 增 `messageId`（成功回显；失败空）。
- 新增 `msg.delivered {messageIds}` / `msg.delivered_res`；`msg.offline_ack` 复用同一处理器。
- 更新 `docs/protocol.md` §17x/§5、`docs/client_api.md`。
- 验收：协议文档一致；`msg.ack` 含 ID。

## T3 交付确认处理器 `applyDeliveryAcks`
- 新函数：单聊 `markDelivered`（0→1，写 `delivered_ts`）；群按 `messageId` 反查 `(group_id,ts,id)`，成员则 `setWatermark(max)`。
- `onOfflineAck` 与新增 `onDelivered` 均调用之。
- **移除**提前推进：`app_group.cpp:617`（在线）与 `app_message.cpp:99`（补发）的 `setWatermark`。
- 指标：`messagesPushed`（推送）与 `messagesDelivered`（确认）分离。
- 验收：群"推送未确认即断开→重登补发"。

## T4 单聊在线确认制（开关）
- `server_config.hpp` 增 `IM_DELIVERY_ACK_REQUIRED`（默认过渡期 `off`）。
- `onSend` 单聊：`off` 保持现状（在线置 1）；`on` 时一律置 `0`（自己对话框仍 1），delivered 仅由确认产生。
- 验收：`on` 时在线未确认即断线→重登补发；`off` 行为不变。

## T5 群离线补发严格游标
- `pushOfflineMessages` 用 `(sinceTs,sinceId)`；初值 `(max(wm,joined-1),0)`；补发不推进水位。
- 验收：同秒多条全达；断开重登从上次确认处续发。

## T6 注册限流真实 IP（#4）
- 键取 `realIp()`（可信代理命中时）else `peerIp()`；应用于 `app_auth.cpp` 注册限流。
- 连接级 `kMaxPerIpConnections` 保持 `peerIp`（握手前无 XFF），文档说明。
- 更新 `docs/nginx-deploy.md`。
- 验收：biz 用 XFF 校验真实 IP 分桶。

## T7 测试与文档
- 单元：游标谓词、`applyDeliveryAcks`、`msg.ack.messageId`。
- `scripts/biz_test.py` 新增：#1 群断线补发、#2 单聊断线补发（`on`）、#3 同秒分页、ack ID、#4 XFF。
- 全量：`ctest` + `test_biz.sh`（内存/Redis）。
- 协议文档与 README 同步。

## T8 客户端仓库跟进（IM-Client）
- 处理 `msg.ack.messageId` → 立即写 `serverKey`（撤回即时可用）。
- `msg.recv`/`msg.offline` 入库后回执 `msg.delivered`（按 messageId 批量，≤1000）。
- 群补发重复按 `messageId` 去重（已有）。
- 联调用例：见 spec §9（A→B、多端、掉线重连、ACK 丢失、推送后立即断、群 3+ 端、同秒多条、撤回+离线）。

## 交付顺序与联调
1. T1–T7 合并 → 服务端 `off` 模式回归。
2. 客户端 T8 发布 → 切 `IM_DELIVERY_ACK_REQUIRED=on` → 真机端到端可靠性联调。
3. 通过后进入 P1（图片收尾 + 1001 维护提示）。
