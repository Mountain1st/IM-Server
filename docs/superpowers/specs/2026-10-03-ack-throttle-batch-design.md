# ACK 限流 + markDelivered 批量化 设计规格

> 状态：**实施中**。范围：任务 ③（审计 P1-4 ACK 处理器无限流、P1-5 `markDelivered` 逐条 UPDATE）。
> 目标仓库：`IM-Server`（基线 `65a2924`）。两个独立逻辑提交：A 准入控制、B 批量更新。

## 1. 背景（审计已确认）

| # | 缺陷 | 证据 |
|---|---|---|
| 1 | `msg.offline_ack` / `group.offline_ack` 无任何限流：恶意客户端可高频空转 DB | `src/app_message.cpp:444`、`src/app_group.cpp:532` 内无 `allow` 调用 |
| 2 | `onRead` 无限流（`chat.*`/`group.*` 查询有限流，read 没有） | `src/app_message.cpp:498` |
| 3 | `markDelivered` 逐条 `UPDATE`：1000 条 ACK = 1000 次 `stmtExec`，全程持同一把仓储锁 | `src/mysql_message_repository.cpp:161-175` |

## 2. 决策

### A. ACK admission control（独立提交 `ack`）

- 新增专用限流桶 `ackLimiter_`（键前缀 `arate:`，与 msg/query/media 桶隔离），额度：
  - **单聊** `msg.offline_ack`：每用户 60 次/10s（正常客户端批量确认，远低于此）。
  - **群** `group.offline_ack`：每用户 60 次/10s。
  - **`msg.read`**：每用户 60 次/10s（可能高频翻页触发，但仍是重查询级别）。
- 超限回：`msg.offline_ack_res` / `group.offline_ack_res` / `msg.read_res` 的 `429 操作过于频繁，请稍后再试`，
  计 `g_metrics.rateLimited`。
- 不做「按 messageIds 数量加权」（保持与既有滑动窗口同一模型；审批边界=请求次数）。
- 注意：ACK/read 是**接收端**路径，均以 `session->userId()` 为键（与发送限流同人不同桶）。

### B. markDelivered 批量化（独立提交 `batch`）

替换逐条 UPDATE 为**有界 IN 列表 + 一次 UPDATE**（`WHERE to_user=? AND status=0 AND message_id IN (...)`）：

- **有界**：单次最多 `kMaxOfflineAckIds=1000` 个 id（handler 已限），SQL 参数用 `stmtExec` 参数化
  （每个 id 一个 `?`，防注入）；不做字符串拼接。
- **单次仓储锁**：一次调用一个批量 UPDATE（而非 N 次），锁持有时间从 O(N) 降为 O(1)。
- **语义保持**（这正是本任务的关键，不能只换 SQL）：
  - `status = 0→1` + `delivered_ts`（谓词 `to_user + status=0 + message_id IN`）；
  - **已 delivered（1）/recalled（3）/不存在**的 id：`IN` 命中 0 行 → 幂等安全（不报错、不重复计数）；
  - 返回 `mysql_affected_rows` 作为本轮真实确认数（指标 `messagesDelivered` 只计真实迁移）。
- 幂等：重复 ACK 同一批 → 第二次 affected=0（不重复计数），行为与旧实现一致。
- 单聊与群消息隔离：群消息 `to_user=0`，天然不被该谓词命中（不变）。

### 非目标

- 不新增协议字段/指令；客户端零改动。
- 不改 `msg.send` 限流、不改群游标推进（`group.offline_ack` 的游标逻辑不动）。
- 不把 ACK 与 read 合并成一个桶（语义不同，分开计费）。

## 3. 测试

**单元**（`tests/mysql_message_repository_test.cpp` 需要 MySQL，不入单测——沿用项目「仓储不单测」先例）：
批量 SQL 语义以 biz 回归覆盖。

**业务回归**（`biz_test.py`）：
- on 模式既有投递段全量回归（单/群 ACK 批次、连续前缀、幂等重复 ACK）；断言 `messagesDelivered`
  指标在重复 ACK 时不增长（经 `/health` 或既有行为断言间接覆盖）。
- 新增：ACK 滥用 429（连发小批 ACK 超过 60 次/10s → 429；`msg.read` 同理）。
- 新增：幂等计数（同 messageId 两次 ACK，第二次不改变 `delivered_ts`）。

## 4. 验收矩阵

| 场景 | 期望 |
|---|---|
| 正常批量 ACK（≤1000） | `markDelivered` 单条 SQL，返回真实迁移数 |
| 重复 ACK 同批 | 幂等（affected=0，指标不重复增长） |
| 混入不存在/已 delivered/recalled id | 该 id 不影响其余、不报错 |
| ACK 高频超限 | `429`（专用桶，不影响 msg.send 额度） |
| `msg.read` 高频 | `429` |
| off/on 双模式 | 全量回归通过 |
