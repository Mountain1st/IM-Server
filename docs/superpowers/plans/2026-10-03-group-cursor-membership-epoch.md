# 群投递游标：成员资格纪元（membership epoch）实现计划

> 依赖规格：`docs/superpowers/specs/2026-10-03-group-cursor-membership-epoch-design.md`。
> 修订既有：`docs/superpowers/specs/2026-10-02-reliable-delivery-design.md` §5（下界/推进语义）。
> 顺序固定：E1→E2→E3→E4。**不触碰** off 模式水位线（独立后续项）与媒体/CI。

## E1 仓储：纪元边界 + 入群时重置游标
- `GroupRepository` 新增 `boundaryCursor(groupId,&ts,&id)`：`SELECT ts,id FROM messages WHERE group_id=? ORDER BY ts DESC,id DESC LIMIT 1`；
  无消息 → `(0,0)`；查询失败 → 返回 false（调用方按错误处理）。
- `addMember` / `addMemberCapped`：在**同一 `mutex_` 临界区**内先取边界，再 INSERT/ON DUPLICATE KEY UPDATE
  同时写 `last_delivered_ts=boundaryTs, last_delivered_id=boundaryId`。
- `createGroup` 建群者入群：游标 `(0,0)`（默认即 0，无需特判；建议显式写 0 以免列默认漂移）。
- 验收：`ctest` 通过；新增 `boundaryCursor` 单测（无消息=0,0；有消息=最大 `(ts,id)`）。

## E2 业务层：补发低界统一为持久游标
- `src/app_message.cpp` `pushOfflineMessages`：
  - 删除 `sinceTs = max(cts, joined-1)` 与 `sinceId` 的 `joined` 条件，改为 `fetchGroupMessages(gid, cts, cid, until, 100, true)`。
  - on/off 均以持久游标为唯一低界。
- `src/app_group.cpp` `onGroupOfflineAck`：逻辑不变（已从 cursor 起）；由 E1 保证新纪元边界正确。
- 验收：T1/T2/T3 用例通过。

## E3 测试：T1–T7 独立用例
- `scripts/biz_test.py` 投递段（on 门禁内）新增 T1–T7（详见 spec §6），每条独立 `check`。
- T4 同秒边界用例需构造「入群前消息」与「入群后消息」同一秒：用较小 `time.sleep` + 显式等待跨秒控制；
  若不稳定则允许 `skipped` 并记录原因（但 T1/T2/T3 必须稳定通过）。
- 移除或调整旧用例中依赖 `joined-1` 下界的隐含假设（若有）。
- 验收：on 模式全绿；off 模式回归不退化（off 段仍 skip delivery）。

## E4 文档与旧 spec 同步
- `docs/protocol.md` / `docs/client_api.md`：明确「游标不跨成员资格纪元」「入群后从入群边界起算」。
- 旧 spec `2026-10-02-reliable-delivery-design.md` §5：
  - 删除/修订 `:68` 的 `max` 推进表述 → 指向连续前缀；
  - 修订 `:75` 的 `(max(wm,joined-1),0)` → 持久游标低界 + 新纪元重置。
- 旧 plan `2026-10-02-reliable-delivery.md` T5 同步加注。
- 验收：文档内部无矛盾；指向本规格。

## 回归命令
```
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build
IM_MYSQL_PASSWORD=... IM_DELIVERY_ACK_REQUIRED=1 python3 scripts/biz_test.py
IM_MYSQL_PASSWORD=... python3 scripts/biz_test.py
```

## 风险与回退
- 存量活跃成员游标 0 → 首次登录大批补发（at-least-once，可接受）。
- 边界捕获与并发写入方向安全（spec §3.2），不永久漏收。
- 回退：E1/E2 均为局部改动，`git revert` 对应提交即可恢复旧行为（旧行为有 P0 缺陷，仅作应急）。
