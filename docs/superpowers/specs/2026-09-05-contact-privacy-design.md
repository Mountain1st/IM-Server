# 联系人功能 + 隐私设置 + 临时对话 设计规格（v2.0 联系人体系）

日期：2026-09-05
状态：已批准（用户确认全部 3 节设计）

## 背景与目标

在现有 IM 服务器（协议 v1.7）基础上新增**联系人体系**与**隐私设置**：

1. 完善联系人功能：搜索用户、发起添加请求、同意/拒绝、联系人列表、删除联系人
2. 设置中新增「同意临时对话」：开启则非联系人可对自己发起临时对话；关闭则非联系人不可对自己发起对话
3. 设置中新增「可被搜索」「可被添加」两个独立开关，为后续名片/群聊添加留出空间
4. 更改发起聊天方式：客户端搜索联系人 → 服务器返回联系人信息 → 用户点击添加
5. 自己对话框统一为 `to==自己 account`（删除 `to==0` 特例），且**自己对话框消息落库**、历史可查

## 已确认决策（来自澄清）

| # | 决策 |
|---|---|
| Q1 | 非联系人 + 目标未开临时对话 → `msg.send` 回 403「对方未开启临时对话，请先添加联系人」，**不落库** |
| Q2 | 新增 `user.search`（模糊搜索账号/昵称，返回列表），保留 `chat.create` 作为精确校验/添加入口 |
| Q3 | 设置存服务器（`users` 表加字段） |
| Q4 | 添加请求持久化到 MySQL（`friend_requests` 表），在线立即推送，离线登录后补发 |
| Q5 | `chat.history` / `msg.read` **不套用**联系人/临时对话权限（建立过会话即可查） |
| Q6 | 联系人操作全量：request/accept/reject/list/requests/delete，含删除 |
| Q7 | 「可被搜索」与「可被添加」**分开实现**（用户后续修正）——两个独立开关 |
| Q8 | 新老用户三开关默认全开（向后兼容） |
| Q9 | `contact.delete` 双向解除 |
| Q10 | 自己对话框统一用 `to==自己 account`（删除 `to==0` 特例） |
| Q11 | 自己对话框消息**落库持久化**，历史可查，只回 ack 不回显 msg.recv |

## 架构方案

采用 **方案 A**（用户确认）：新增独立 `ContactRepository` 模块，仿现有 `UserRepository`/`MessageRepository` 分层模式。`msg.send` 权限校验为纯服务端逻辑。

## 数据库变更

### 1. `users` 表加三个字段（默认全开=向后兼容）

```sql
ALTER TABLE users ADD COLUMN allow_temp_chat TINYINT NOT NULL DEFAULT 1; -- 允许非联系人临时对话
ALTER TABLE users ADD COLUMN searchable      TINYINT NOT NULL DEFAULT 1; -- 可被搜索（user.search）
ALTER TABLE users ADD COLUMN allow_add       TINYINT NOT NULL DEFAULT 1; -- 可被添加（contact.request）
```

### 2. 新增 `contacts` 表（双向各一条，对称查询，删除时删两条）

```sql
CREATE TABLE IF NOT EXISTS contacts (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id    BIGINT UNSIGNED NOT NULL,
  contact_id BIGINT UNSIGNED NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_pair (user_id, contact_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

### 3. 新增 `friend_requests` 表（持久化 + 离线补发）

```sql
CREATE TABLE IF NOT EXISTS friend_requests (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  from_user  BIGINT UNSIGNED NOT NULL,
  to_user    BIGINT UNSIGNED NOT NULL,
  status     TINYINT NOT NULL DEFAULT 0,  -- 0=pending 1=accepted 2=rejected
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_from_to (from_user, to_user)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

## 三个独立开关

| 开关 | 控制 | 关闭时效果 |
|---|---|---|
| `allow_temp_chat` | `msg.send` 非联系人 | 非联系人发消息 → 403「对方未开启临时对话，请先添加联系人」（不落库） |
| `searchable` | `user.search` | 搜不到该用户 |
| `allow_add` | `contact.request` | 添加被拒 → 403「对方未开启添加」 |

> `chat.create`（精确校验/添加入口）不受三个开关影响：仅用于客户端发起对话前校验目标存在性与获取昵称，不产生联系人关系。搜索/添加的权限由 `user.search` 与 `contact.request` 各自按 `searchable`/`allow_add` 把关。

后续扩展预留：名片/群聊添加可分别按 `allow_add` 或独立开关校验。

## msg.send 权限规则

```
resolveTarget(to) 解析目标 userId：
  0 / 空 / 非法       → 400 参数错误（不再有 0=自己 语义）
  to == 自己 account  → 允许（自己对话框）
  是联系人            → 允许
  目标 allow_temp_chat == 1 → 允许（临时对话）
  否则                → msg.ack 403「对方未开启临时对话，请先添加联系人」（不落库）
```

### 自己对话框（`to==自己 account`）
- 允许发送
- **落库 MySQL**（from_user = to_user = 自己 userId，chat_type=single，status=delivered）
- 回 `msg.ack code=0`
- **不回显 msg.recv**（发送方即自己，客户端本地显示）
- clientMsgId 幂等去重 / 限流照常生效

### chat.history 自己对话框（`to==自己 account`）
- 返回自己发给自己的全部消息（`from_user=自己 AND to_user=自己`），按 ts 倒序

### msg.read（`from==自己 account`）
- 直接成功（自己消息落库即视为已读/无未读）

## 协议指令与事件

### 新增 C2S 指令

| 指令 | data | 说明 |
|---|---|---|
| `user.search` | `{ "keyword": "...", "limit": 20(可选) }` | 模糊搜索账号/昵称（仅 `searchable=1` 且非自己）；回 `user.search_res` |
| `contact.request` | `{ "account": "..." }` | 发起添加；目标 `allow_add=0` 回 403、已是联系人回 409、已有 pending 回 409；回 `contact.request_res`，推送 `contact.request` 给对方 |
| `contact.accept` | `{ "account": "..." }` | 同意 pending 请求；双方成联系人；回 `contact.accept_res`，推送 `contact.added` 给请求方 |
| `contact.reject` | `{ "account": "..." }` | 拒绝请求（status→rejected）；回 `contact.reject_res` |
| `contact.list` | `{}` | 我的联系人列表；回 `contact.list_res` |
| `contact.requests` | `{}` | 收到的 pending 请求列表；回 `contact.requests_res` |
| `contact.delete` | `{ "account": "..." }` | 双向解除；回 `contact.delete_res`，推送 `contact.deleted` 给对方 |
| `settings.get` | `{}` | 获取三个开关；回 `settings.get_res` |
| `settings.set` | `{ "allowTempChat": bool, "searchable": bool, "allowAdd": bool }` | 更新开关（缺省字段不修改）；回 `settings.set_res` |

### 新增 S2C 事件

| 事件 | data | 触发 |
|---|---|---|
| `contact.request` | `{ "from": "<account>", "fromNick": "...", "ts": ... }` | 收到添加请求（在线立即推；离线登录后补发） |
| `contact.added` | `{ "account": "<account>", "username": "..." }` | 我的请求被同意，双方成为联系人 |
| `contact.deleted` | `{ "account": "<account>" }` | 关系被解除（双向） |
| `user.search_res` | `{ "code", "data": { "users": [{ "account", "username", "isContact" }] } }` | 搜索结果 |
| `contact.*_res` | `{ "code", "msg", "data" }` | 各指令响应（复用 makeResult 结构） |

### 错误码新增

| code | 场景 |
|---|---|
| 403 | 对方未开启临时对话 / 对方未开启添加 |
| 409 | 已是联系人 / 请求已存在 |

### 离线补发
- `contact.request` 持久化（status=pending）；在线立即推送，离线登录后补发（与 `msg.offline` 同机制，客户端 accept/reject 闭合）

## 服务端模块结构

### 新增 `src/contact_repository.{hpp,cpp}`
- 方法：`isContact` / `addContactPair`（双向）/ `deleteContactPair`（双向）/ `listContacts` / `addRequest` / `acceptRequest` / `rejectRequest` / `listPendingRequests` / `getSetting` / `setSetting`
- 内存缓存 `std::unordered_set<std::pair<int64_t,int64_t>>` 加速 `isContact` 热路径（仿 `ids_` 缓存），登录/成对/删除时刷新
- 内部互斥锁，线程安全

### `src/user_repository.*` / `mysql_user_repository.*`
- `fetchSettings(userId, &allowTemp, &searchable, &allowAdd)`
- `updateSettings(...)`
- `searchUsers(keyword, excludeUserId, &results)`（LIKE，仅 searchable=1）

### `src/appserver.cpp` / `.hpp`
- 新增处理函数：`onSearch` / `onContactRequest` / `onContactAccept` / `onContactReject` / `onContactList` / `onContactRequests` / `onContactDelete` / `onSettingsGet` / `onSettingsSet`
- `onSend` 增加权限校验
- `pushOffline` 扩展：登录补发 pending 的 `contact.request`
- `onHistory` 支持自己对话框（不再空历史）
- `onRead` 支持自己对话框
- 新增成员 `std::unique_ptr<ContactRepository> contactRepo_`

### CMakeLists.txt
- 新增 `contact_repository.cpp`（主体编译）

## 测试计划

- `scripts/biz_test.py` 扩展（72 → ~90 用例）：
  - 联系人生成：A 搜 B → A request B → B 收到 contact.request → B accept → 双方联系人、A 收到 contact.added
  - 临时对话：非联系人 + 目标开临时对话 → 可发；目标关临时对话 → 403 不落库
  - 搜索开关：searchable=0 → 搜不到
  - 添加开关：allow_add=0 → request 403
  - 自己对话框：`to==自己 account` 落库 → chat.history 可查、不回显 msg.recv
  - 删除联系人：双向解除、删除后无法再发（目标关临时对话时 403）
  - 更新既有 `to:0` 用例为 `to==自己 account`
- 双模式回归（内存 + Redis）
- CTest 新增 `contact_repository_test`（可选）

## 文档更新

- `docs/protocol.md`（v1.8）：新指令/事件/错误码、自己对话框 `to==0` 移除说明、三开关说明
- `docs/client_api.md`：新指令接入指引、搜索→添加→对话流程
- `docs/server-architecture-learning-guide.md`：新增 ContactRepository 模块说明
- 同步 `docs/docs/` 副本
- `db/schema.sql`：新增表 + users 字段变更