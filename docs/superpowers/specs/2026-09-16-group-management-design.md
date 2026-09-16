# 群聊管理设计（v2.0，2026-09-16）

## 1. 背景
v1.9 群聊只有建群/拉人/退群/列表/成员/历史，缺管理闭环，且群主不能退群又无转让/解散，
群主被永久锁定。本规格补齐 5 功能：踢人、转让群主、解散群、改群名、群公告。

## 2. 范围（用户已确认）
- 踢人：群主移除成员，被踢可被重新拉入，重入后仅看新窗口历史（与退群一致）
- 转让：只能转给现群成员，原群主变为普通成员（之后可退群）
- 解散：群主解散，成员保留解散前窗口历史可查（与退群一致）
- 改群名：仅群主，非空且长度 ≤ 32（同建群规则）
- 群公告：单条最新，群主发布/清空（空 content=清空），成员可看，随 `group.list` 下发

## 3. 新指令（C2S → S2C）
| 指令 | data | 成功响应 | 权限 |
|---|---|---|---|
| `group.kick` | `{groupId, account}` | `group.kick_res{}` | 仅群主；不能踢群主本人（409）；目标不存在 404，非成员 409 |
| `group.transfer` | `{groupId, account}` | `group.transfer_res{}` | 仅群主；目标不存在 404，存在但非成员 409 |
| `group.dissolve` | `{groupId}` | `group.dissolve_res{}` | 仅群主 |
| `group.rename` | `{groupId, name}` | `group.rename_res{}` | 仅群主；name 非空≤32（400） |
| `group.announce` | `{groupId, content}` | `group.announce_res{}` | 仅群主；content ≤ 1024 字节（超限 413），空=清空 |

通用错误：`400 参数错误`（groupId 缺失/非整数）、`404 群不存在`、解散后操作（除 history）`404 群已解散`、
非群主操作 `403 仅群主可操作`。

## 4. 存储变更（db/schema.sql，复用模型、无新表）
- `groups` 新增：`announcement TEXT DEFAULT ''`、`announce_ts BIGINT DEFAULT 0`、
  `dissolved TINYINT DEFAULT 0`、`dissolved_ts BIGINT DEFAULT 0`
- 踢人：复用退群路径（置 `group_members.left_ts`，`is_owner=0` 行不受影响）
- 转让：`UPDATE groups SET owner_id=新成员`
- 解散：`UPDATE groups SET dissolved=1, dissolved_ts=now`（成员行不动，窗口保留）
- 改名/公告：`UPDATE groups SET name/announcement`

## 5. 事件推送（S2C）
- 踢人：被踢在线者收 `group.removed{groupId}`，其余在线成员收 `group.member_changed{groupId, change:"kick", account}`
- 转让：在线成员收 `group.owner_changed{groupId, account:新群主}`
- 解散：在线成员收 `group.dissolved{groupId}`
- 改名：在线成员收 `group.renamed{groupId, name}`
- 公告：在线成员收 `group.announced{groupId}`（正文走 `group.list` 拉取，避免大包扇出）
- `group.list` 条目新增 `announcement`、`announceTs` 字段（已解散群不列出）

## 6. 解散后行为
- `group.list`：不再列出已解散群
- `msg.send(group)` / `group.invite` / `group.members` / `group.rename` / `group.announce`：`404 群已解散`
- `group.history`：曾成员仍可查在群窗口（SQL 加 `dissolved` 豁免）
- `group.leave` on 已解散群：幂等成功（行已是离开态则直接 ok）

## 7. 测试计划（biz_test.py，沿 v1.9 模式）
- 踢人：群主踢成员 ok（成员收 removed、他人收 kick 事件、被踢发消息 403、重拉 ok 且只见新窗口）
- 踢人权限：非群主 403、踢群主本人 409、踢非成员 404、踢已解散群 404
- 转让：转成员 ok（owner_changed 事件、原群主变成员可退群、新群主可拉人）
- 转让权限：非群主 403、转非成员 404、转已退群者 404
- 解散：解散 ok（dissolved 事件、list 不再列出、发送 404、history 仍可查窗口）
- 改名：改名 ok（renamed 事件、超长/空 400、非群主 403）
- 公告：发布/清空 ok（announced 事件、list 下发正文、超限拒绝、非群主 403）
- 目标：用例数 150+，内存/Redis 双模式全绿，ctest 全绿

## 8. 非目标
- 管理员角色、多条公告列表、入群审批、群禁言、群文件——均不在本期范围
