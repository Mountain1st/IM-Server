# ACK 限流 + markDelivered 批量化 实现计划

> 依赖规格：`docs/superpowers/specs/2026-10-03-ack-throttle-batch-design.md`。
> **两个逻辑提交**：A ACK 准入；B 批量更新。

## A `ack: throttle offline acknowledgements and read receipts`
- `server_config.hpp`：`kAckWindow(10s)`、`kMaxAckPerWindow=60`、`kMaxReadPerWindow=60`
  （可合并为同一常量；保持语义独立注释）。
- `appserver.hpp`：`ackLimiter_`（键前缀 `arate:`）。
- `appserver.cpp`：构造装配 `ackLimiter_(makeRateLimiter(redisConfig, "ACK 限流", "arate:"))`。
- `onOfflineAck`：入口限流（`msg.offline_ack_res` 429）。
- `onGroupOfflineAck`：入口限流（`group.offline_ack_res` 429，未支持命令也在限流后校验参数顺序——
  顺序取「类型/登录 → 限流 → 参数」，与 send 先例一致？**取参数前限流**防畸形包绕开）。
- `onRead`：入口限流（`msg.read_res` 429）。
- 测试：biz 新增 429 用例（三处）；离线/群投递段回归。
- 验收：ctest 全绿；biz 双模式。

## B `msg: batch delivery updates into single bounded UPDATE`
- `mysql_message_repository.cpp` `markDelivered`：
  - 空列表直接 0；
  - 参数化 `UPDATE messages SET status=?, delivered_ts=? WHERE to_user=? AND status=? AND message_id IN (?,?,...)`；
  - 返回 `mysql_stmt_affected_rows`。
- 测试：biz 幂等/混入脏 id 断言；投递段全量回归。
- 验收：ctest 全绿；biz 双模式。

## 回归命令
```
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build
IM_MYSQL_PASSWORD=... python3 scripts/biz_test.py
IM_MYSQL_PASSWORD=... IM_DELIVERY_ACK_REQUIRED=1 python3 scripts/biz_test.py
```
