# 群已读与群未读 设计规格

> 状态：**实施中**。范围：审计 P1-4（群无真实未读/已读；`conversation.list` 群条目占位恒 0）。
> 目标仓库：`IM-Server`（基线 `dbc8ad4`）。为 v3.4 能力，不改投递确认语义。

## 1. 背景

- `conversation.list`（v3.2）群条目 `unreadCount`/`lastReadTs` 恒 0（占位）——代码注释与协议均自认。
- `group_members.last_delivered_(ts,id)` 是**投递**游标（收到 ≠ 读过），不可当已读游标（审计已确认）。
- 群聊缺少成员级已读状态；客户端无法渲染群未读角标，也无「一键已读」回执。

## 2. 决策

### D1 成员级已读游标（不引入全员回执）

- `group_members` 新增 `last_read_ts BIGINT NOT NULL DEFAULT 0`、
  `last_read_id BIGINT UNSIGNED NOT NULL DEFAULT 0`（迁移幂等，与 `last_delivered_*` 并列）。
- 语义：该成员已阅该群消息流至 `(last_read_ts,last_read_id)`——**用户级（账号级）非设备级**，
  任一设备已读即推进（与投递游标同先例）。
- **不做**「每条消息全员已读列表」（发送者可见谁读了）——那是另一个产品能力，本规格非目标。

### D2 已读推进指令 `group.read`

```
C→S: { "type": "group.read", "data": { "groupId": 12, "messageId": "1720000000-42" } }
S→C: { "type": "group.read_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```

- `messageId`：客户端当前**最后一条可见消息**的 ID（来自 `msg.recv`/历史/`conversation.list.lastMessageId`）。
  服务端反查其 `(ts,内部id)` 作游标——**严格 (ts,id)，无同秒歧义**（ts-only 会把同秒后到消息误判已读）。
- 校验：群存在且未解散（否则 404）、现成员（否则 403）、`messageId` 存在（否则 404）、
  属于该群（否则 400）。
- 推进 CAS **只进**：`(newTs,newId)` 字典序 > 旧游标才更新（重复/回退 `group.read` 幂等）。
- 限流：复用 `msg.read` 的 `rrate:` 桶（60 次/10s）。
- **免 ACK**：`group.read` 是状态声明，丢失由下次进入会话重发/列表拉取自愈（与 `msg.read` 同先例）。

### D3 多端同步 `group.read_notify`

- 推进成功后向该用户**其他在线端**推送 `{ "type": "group.read_notify", "data": { "groupId": 12, "readTs": 1720000000, "messageId": "..." } }`
  （发起端不收，与 `msg.read_notify` 同序：先回执后扇出）。
- 不向群内其他成员推送（无已读回执语义）。

### D4 群未读 = 可用事实派生（无物化计数）

`conversation.list` 群条目改为真实值：

- `lastReadTs` = 成员 `last_read_ts`。
- `unreadCount` = 该群中满足全部条件的消息数：
  1. `(ts,内部id) > (last_read_ts,last_read_id)`（未读游标之后）；
  2. `from_user != me`（自发不计未读）；
  3. `status != RECALLED(3)`（撤回退出未读，与单聊 `status IN (0,1)` 同语义）；
  4. `ts >= joined_ts`（成员资格窗口内；入群前历史不可见，不计未读）。
- 计数逐群点查（会话数已有界：`limit ≤ 100`），索引 `idx_group_ts(group_id, ts)` 可用。

### D5 纪元/重入兼容

- 重入不重置已读游标；未读窗口取「已读游标之后 **且** 入群窗口之内」的交集——
  旧纪元的已读位置不会把新纪元消息误判为已读（窗口条件兜底）。

## 3. 非目标

- 不改变 `last_delivered_*` 投递游标语义（两者独立：delivered ≠ read）。
- 不向发送者暴露「谁已读」（全员回执）。
- 不改单聊 `msg.read`（ts-only 先例保持）。
- 不在 `group.offline_ack` 中隐式推进已读（确认收到 ≠ 已读）。

## 4. 测试

**业务回归**（`biz_test.py` 会话段改造 + 新增群已读用例）：

| 场景 | 期望 |
|---|---|
| 他人群消息 → 列表 | 群 `unreadCount ≥ 1`，`lastReadTs==0` |
| 撤回该消息 → 列表 | 未读减少（撤回退出未读） |
| 自己群发 → 自己列表 | 不计未读；对端列表计 1 |
| `group.read{messageId}` | 回 0；列表 `unreadCount==0`、`lastReadTs>0` |
| 另一端在线（token 二端） | 收 `group.read_notify{groupId,readTs}` |
| 非成员调用 | 403；群不存在/已解散 404 |
| 伪造/跨群 messageId | 404 / 400 |
| 重复/回退 `group.read` | 幂等 0，游标不回退 |
| 入群前历史 | 不计未读（joined 窗口） |

## 5. 验收命令

```
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build
IM_MYSQL_PASSWORD=... python3 scripts/biz_test.py                 # off
IM_MYSQL_PASSWORD=... IM_DELIVERY_ACK_REQUIRED=1 python3 scripts/biz_test.py  # on
```
