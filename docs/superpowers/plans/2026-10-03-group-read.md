# 群已读与群未读 实现计划

> 依赖规格：`docs/superpowers/specs/2026-10-03-group-read-design.md`。
> 顺序：G1 → G2 → G3 → G4。无功能开关（客户端不用新指令则行为不变，未读角标仅展示增强）。

## G1 Schema 与仓储
- `db/migrate_group_read.sql`：幂等加 `group_members.last_read_ts`、`last_read_id`（仿 `migrate_delivery_cursor.sql`）。
- `db/schema.sql` 同步两列。
- `GroupRepository`：`getReadCursor(gid,uid,&ts,&id)`、`advanceReadCursor(gid,uid,ts,id)`
  （CAS `((last_read_ts<?) OR (last_read_ts=? AND last_read_id<?))` 只进）。
- `MessageRepository` / `MySqlMessageRepository`：`countGroupUnread(gid,uid,readTs,readId,joinedTs)`
  （谓词见规格 D4；预处理参数）。
- 验收：`ctest` + 迁移可重复执行。

## G2 业务：`group.read` + 多端通知
- `AppServer::onGroupRead`（`app_group.cpp`）+ 路由（`appserver.cpp`）+ 声明（`appserver.hpp`）。
- 流程：`readLimiter_` 限流 → 参数（groupId int、messageId 非空串）→ 群存在/未解散 → 现成员 →
  `fetchMessageCursor` 反查（404/400 分类）→ `advanceReadCursor`（失败 500）→ 回执 →
  `group.read_notify` 扇出该用户其他在线端。
- 验收：biz 群已读用例（含幂等、跨群、非成员）。

## G3 会话列表群未读
- `onConversationList` 群装配：`getReadCursor` + `memberPeriod(joined)` + `countGroupUnread` →
  真实 `unreadCount`/`lastReadTs`；更新代码注释（去掉「恒 0 占位」）。
- 验收：biz 会话段断言替换（撤回退出未读、自己不计、read 归零）。

## G4 文档
- `docs/protocol.md`：升 v3.4；新增 `group.read`/`group.read_notify`/`group.read_res`；
  `conversation.list` 行与响应示例群字段改真实语义；`group.read` 约束小节。
- `docs/client_api.md`：新增 §5.35 群已读；§5.33 `conversation.list` 群字段说明更新。
- 验收：文档无「占位恒 0」残留表述。

## 回归命令
```
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build
IM_MYSQL_PASSWORD=... python3 scripts/biz_test.py
IM_MYSQL_PASSWORD=... IM_DELIVERY_ACK_REQUIRED=1 python3 scripts/biz_test.py
```
