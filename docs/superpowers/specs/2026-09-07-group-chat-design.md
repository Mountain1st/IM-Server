# 群聊功能（最小闭环）设计规格

日期：2026-09-07
状态：已批准（用户确认 Q1–Q12 与 §1–§4 设计）
协议版本：v1.9（新增）

## 背景与目标

在单聊 + 联系人体系（v1.8/v1.8.1）基础上新增群聊最小闭环：建群、群主拉人、群内发消息、群历史、退群。`chatType` 字段（协议 + DB `chat_type` 列）早已预留 `"group"` 值，本次正式启用。

## 已确认决策

| # | 问题 | 决策 |
|---|---|---|
| Q1 | 核心范围 | A：最小闭环（建群 + 拉人 + 群发消息 + 群历史 + 退群；无踢人/转让/公告/改名） |
| Q2 | 群寻址 | A：数字 `groupId`（自增主键，与 userId 命名空间独立，`chatType: "group"` 区分） |
| Q3 | 建群拉人流程 | B：`group.create {name}` 建空群（仅群主）+ `group.invite` 逐个拉，被拉人直接进群 |
| Q4 | 拉人资格 | A：仅群主可拉（非群主→403） |
| Q5 | 人数上限 | A：50 人（含群主） |
| Q6 | 发言权限 | A：仅现成员可发；与单聊临时对话逻辑解耦 |
| Q7 | 历史范围 | B：按在群时间过滤（新成员看加入后；退群成员可拉在群期间；重入以最新 join 为准） |
| Q8 | 群主退群 | B：群主调 `group.leave` 回 403（本次无转让，群主退不了） |
| Q9 | 群已读 | A：不做；status 只到 delivered；客户端不对群发 `msg.read`（契约） |
| Q10 | 成员变更通知 | A：`group.member_changed` 在线推送全群；离线者靠 `group.members` 拉取 |
| Q11 | 群名 | A：建群必填 1–32 字符（可重复），不可改名 |
| Q12 | 拉人隐私校验 | A：不检查对方 allowAdd（联系人关系已过该关；群邀请与添加请求解耦） |

## 架构方案

用户选用**方案二：单行 + 按群查**（未采纳推荐的 fan-out 方案一）：
- 群消息在 messages 表只写**一行**（`from=发送者, to_user=0, chat_type='group', group_id=G`）
- per-member 投递状态由水位线 `group_members.last_delivered_ts` 表达（推送/补发时推进）
- 代价：离线补发、历史查询需按 `group_id + 时间` 过滤（方案一复用的单聊行级机制此处不适用）；收益：无写放大，存储省

## §1 存储与数据模型

**新表：**
```sql
groups(group_id BIGINT 自增PK, name VARCHAR(32) NOT NULL, owner_id BIGINT NOT NULL, created_ts BIGINT)
group_members(group_id, user_id, joined_ts, left_ts NULL, last_delivered_ts DEFAULT 0, UNIQUE(group_id,user_id))
```

**messages 表扩展：** 加 `group_id BIGINT DEFAULT 0` + `KEY idx_group_ts(group_id, ts)`；`saveMessage` 加 `groupId` 参数（默认 0）；单聊三查询（history/offline/markRead）加 `AND group_id = 0` 隔离。

**水位线语义：** 在线推送与登录补发时推进；补发条件 `ts > watermark AND ts >= joined_ts`（退群成员再加 `ts <= left_ts`）；history 拉取不推进水位。

**退群/重入：** leave 置 `left_ts`（行保留）；重入重置 `joined_ts=now, left_ts=NULL`（旧时段因 `ts >= joined` 自然不可见）。

## §2 协议指令与事件

| 指令 | 说明 |
|---|---|
| `group.create {name}` | 建群（群主自动入群）→ `group.create_res {groupId, name}` |
| `group.invite {groupId, account}` | 仅群主；对方须是群主联系人；50 人上限；已在群→409；不存在→404 |
| `group.leave {groupId}` | 群主→403；非成员→403 |
| `group.list {}` | 我的群 `{groups: [{groupId, name, memberCount, isOwner}]}` |
| `group.members {groupId}` | 仅现成员可拉 `{members: [{account, username, joinedTs, isOwner}]}` |
| `group.history {groupId, limit}` | 成员查在群期间；退群查其在群期间；从未加入→403 |

**推送：** `group.added {groupId, name, inviter}`（被拉人在线；离线靠 `group.list` + `msg.offline` 自洽）；`group.member_changed {groupId, change: join/leave, account}`（在线成员）；群消息体 `{from, fromNick, groupId, groupName, content, ts, clientMsgId, messageId, chatType: "group"}`。

## §3 发送 / 补发 / 历史流程

**onSend 群分支**（`chatType=="group"` 先行）：`to` 须为整数 groupId（account 字符串→404）；群不存在→404；非现成员→403；走发送方幂等 + 限流；写单行；在线成员（除发送者）逐会话推 `msg.recv` 并推进水位；发送者不回显。

**登录补发**：单聊 pending 行 + 各群按水位线条件查行 → 按 ts 合并为一个 `msg.offline`（沿用总额度上限）；补发后推进水位至本次最大 ts。

**群历史**：`group_id=G AND ts >= joined（退群加 ts <= left）ORDER BY ts DESC LIMIT`，走 `idx_group_ts`。

## §4 权限与边界

- 发言仅现成员；拉人仅群主；退群群主 403；群不做已读（客户端契约）；单聊视图永不混入群行（`AND group_id = 0`）
- App 结构：新 `app_group.cpp` + `GroupRepository`（成员集合内存缓存，isMember 热路径零 DB；水位线走 DB），沿用既有分层与互斥模式

## 测试计划

- biz 新增：建群/拉人/群发/群收/群历史/退群/权限矩阵（非成员 403、非群主拉人 403、群主退群 403、50 人上限、重入历史语义、退群成员历史）
- 双模式回归（内存 + Redis）+ origin + CTest

## 文档更新

- `docs/protocol.md`：v1.9 条目 + 群指令/事件/错误码表
- `docs/client_api.md`：群聊接入章节
- 同步 `docs/docs/` 副本
