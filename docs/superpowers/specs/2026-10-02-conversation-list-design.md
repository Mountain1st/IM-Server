# 会话列表（conversation.list）设计（v3.2）

日期：2026-10-02
状态：已批准（方案 A 派生式；经用户审查修正群未读语义后定稿）

## 1. 目标

补齐产品层缺口：登录后"我有哪些会话、各多少未读"。与 chat.history（打开会话后取历史）正交。

## 2. 裁定记录

| 决策点 | 裁定 | 理由 |
|---|---|---|
| 架构 | **方案 A 派生式**（无 conversations 表） | 未读单聊语义已编码在 `messages.status`；物化表是第二事实源，send/offline_ack/read/recall 4 条写路径对齐成本高、必漂移；当前规模（保留期清理 + 有界 limit）派生成本可控。物化视图是规模化后的演进路径（评审叙事保留） |
| **群聊未读** | **v3.2 不提供真实值：字段存在、恒 0，协议明确"群已读能力尚未实现"** | `group_members.last_delivered_ts` 是**投递水位线**（在线投递即推进），不是已读游标——拿它算 unread 得到的是"未投递数"，语义错误（在线用户收 10 条未点开 → 真实未读 10，水位线公式得 0）。群已读是独立能力（`group_members` 加 `last_read_ts` + `group.read` + `group.read_notify`），不为本功能提前拖入 |
| lastReadTs | 单聊=真实值（收方向 `MAX(read_ts)`）；**群聊恒 0（保留字段）** | 同上，不假装群已读存在 |
| lastMsgRecalled | 保留（末条消息 status==3 → true） | 与 v3.1 撤回闭环：撤回消息自然从 `SUM(status IN (0,1))` 消失、末条撤回可渲染「撤回了一条消息」；客户端**优先判 recalled 再判 msgType** |
| 索引 | 新增 `idx_recv_peer (to_user, from_user, ts)` | 现有 `idx_pair_ts (from_user, to_user, ts)` 只覆盖"我发"方向；补"我收"方向聚合 |
| 末条查询 | 有界 N+1（每会话 1 次索引点查，`ORDER BY ts DESC, id DESC LIMIT 1`） | ≤limit 次点查，清晰可讲；同秒边界用 (ts,id) 严格序消除 |
| 自己对话框 | 排除（to==self 不进列表） | 客户端现状不渲染，YAGNI |
| 限流 | `queryLimiter_`（与 history 同桶） | 查询类操作先例 |
| 排序 | lastMsgTs 倒序，limit 缺省 50 上限 100 | — |

## 3. 数据流（v3.2）

```text
conversation.list {limit?}
  1) 单聊聚合（1 条 SQL，双向合并）：
     收: SELECT from_user, MAX(ts), SUM(status IN (0,1)), MAX(read_ts)
         FROM messages WHERE to_user=me AND group_id=0 AND from_user!=me
         GROUP BY from_user                      -- idx_recv_peer
     发: SELECT to_user, MAX(ts)
         FROM messages WHERE from_user=me AND group_id=0 AND to_user!=me
         GROUP BY to_user                        -- idx_pair_ts 前缀
     合并：lastMsgTs=双向 MAX；unread=收方向；lastReadTs=收方向 MAX(read_ts)
  2) 群聚合：我的现群 × GROUP BY group_id 取 MAX(ts)（1 条 SQL）
  3) 候选合并 → lastMsgTs 倒序 → 截 limit
  4) 幸存者逐个末条点查（≤limit 次）：
     单聊 WHERE ((a,b) OR (b,a)) ORDER BY ts DESC,id DESC LIMIT 1
     群   WHERE group_id=g   ORDER BY ts DESC,id DESC LIMIT 1
     → lastMessageId / lastMsgType / lastMsgRecalled(status==3)
  5) 装配（peer account/nick、groupName）→ conversation.list_res
```

## 4. 协议（v3.2 §5.33）

```
conversation.list {limit?: 50, 上限 100}
→ conversation.list_res {code, data:{conversations:[{
     chatType: "single"|"group",
     account|groupId, nick?|groupName?,
     lastMessageId, lastMsgTs, lastMsgType, lastMsgRecalled,
     unreadCount,        // 单聊=真实未读；群聊恒 0（群已读未实现，见字段细则）
     lastReadTs          // 单聊=已读游标；群聊恒 0
   }]}}
```

- 限流 429（查询桶）；未登录 401。
- 字段细则：群 `unreadCount`/`lastReadTs` 为 v3.2 保留占位（0），群已读能力上线（`group_members.last_read_ts` + `group.read`/`group.read_notify`）后启用真实值；客户端不得据此渲染群未读角标。

## 5. 已知边界（记录不修）

- 群消息/水位线均为 Unix 秒，`ts > cursor` 存在同秒边界问题（群补发"微重复"技术债同源）。未来做群已读/同步水位线时演进为 `(ts, id)` 复合游标或严格单调 sequence——本功能末条查询已用 `(ts,id)` 严格序，不受影响。

## 6. 任务切分

| # | 任务 |
|---|---|
| T1 | `idx_recv_peer` 迁移 + schema 同步；repo：`ConversationEntry/fetchSingleConversations`、`LastMessageInfo/fetchLastMessageSingle/fetchLastMessageGroup`、群 lastTs 聚合 |
| T2 | `onConversationList` + 分发注册 + 常量（limit 缺省/上限）+ 装配排序截断 |
| T3 | biz ~10 用例：多会话排序/单聊 unread/msg.read 归零/recall 减 unread/recall→lastMsgRecalled/群会话(unread=0,lastReadTs=0)/自己对话框排除/limit/退群排除/列表无越权泄漏 |
| T4 | protocol v3.2 + client_api 同步 + 终验合并推送 |
