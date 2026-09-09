# IM 客户端接入标准（client_api）

> 面向 **Qt 客户端（QWebSocket）** 与 **Web 客户端（浏览器 WebSocket）** 的开发接入文档。
> 客户端实现只依赖本文件即可；协议正文（字段细则、时序、变更历史）见 [protocol.md](./protocol.md)。

**服务端地址**：`ws://<host>:9191/`（默认端口 9191，路径 `/`；后续经 Nginx 提供 `wss://`）

---

## 1. 快速开始

| 步骤 | 客户端动作 | 服务端回包 |
|---|---|---|
| 1. 连接 | 建立 WebSocket 连接 | 握手成功 |
| 2. 注册（可选） | 发送 `auth.register`（含 `account`） | `auth.register_res` |
| 3. 登录 | 发送 `auth.login`（用 `account`） | `auth.login_res` |
| 4. 聊天 | 发送 `msg.send`（`to` 用目标 `account`） | `msg.ack` + 对方 `msg.recv` |
| 5. 登出 | 发送 `auth.logout` | `auth.logout_res` |

## 2. 用户模型（客户端必读）

| 字段 | 类型 | 用途 | 客户端用法 |
|---|---|---|---|
| `userId` | number | 数字唯一标识（服务器内部） | **对客户端不透明**：不用于任何指令/事件字段，可忽略 |
| `account` | string | 登录账号（注册/登录凭据，唯一，**10-11 位纯数字**） | 注册、登录、**消息收发定位目标**（`msg.send.to`、`chat.history.to`、`msg.read.from`）与**联系人/搜索/添加定位目标**（`user.search`、`contact.*` 均用 `account`）均用 `account` |
| `username` | string | 昵称（展示名，可重复，缺省=account） | 界面展示；消息中由 `fromNick` 直接携带 |
| `groupId` | number | 数字群标识（群收发定位目标，命名空间独立） | 群聊专用：`msg.send {chatType: "group"}` 的 `to`、群指令/事件的 `groupId` 均用它；建群时分配的数字自增 id，与 `userId` 无关，按 `chatType` 区分单聊/群聊 |

**三者勿混用**：登录、收发消息、历史、已读全部用 `account`；`userId` 对客户端不透明（服务器内部用）；气泡展示用 `username`/`fromNick`。**群聊会话键用数字 `groupId`**（单聊会话键为对方 `account`），两者不会冲突。

## 3. WebSocket 层要求

- 文本帧（`opcode=0x1`），消息体为 **UTF-8 编码的单个 JSON 对象**；**客户端发送的帧必须掩码（MASK）**，服务器拒绝未掩码帧
- **握手要求**：请求行必须为 `GET / HTTP/1.1`；携带 `Host` 头；`Sec-WebSocket-Key` 为合法 Base64；`Upgrade: websocket`、`Connection: Upgrade`、`Sec-WebSocket-Version: 13`；不满足会被断开（QWebSocket / 浏览器 / beast 默认均满足）
- **自动回 Pong**：服务端每 30 秒发一次 `Ping` 帧，客户端必须回 `Pong` 帧，否则 60 秒内无任何数据会被服务端断开
- **握手须在 10 秒内完成**，否则服务端断开连接（客户端应在连接后立即发握手请求）
- 所有响应均为服务端主动推送；客户端 **不轮询**
- **健康检查**：`GET http://host:9191/health` 返回运行指标 JSON（连接数、握手失败、协议错误、认证失败、限流、投递数等），供运维探活

## 4. 信封格式

每次发送/接收都是一个 JSON 对象（信封）：

```json
{ "type": "msg.send", "seq": 1, "ts": 1720000000, "data": {} }
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `type` | string | 是 | 指令/事件名（见 §5、§6） |
| `seq` | number | 否 | 客户端自增序号，建议从 1 递增 |
| `ts` | number | 否 | 发送方 Unix 时间戳（秒） |
| `data` | object | 是 | 业务参数 |

> **必须忽略接收到的信封及 data 中的未知字段**，以保证协议向后兼容。

## 5. 客户端 → 服务端指令

### 5.1 注册 `auth.register`

请求（`username` 为昵称，可选，缺省=account）：
```json
{ "type": "auth.register", "seq": 1, "data": { "account": "13800138000", "password": "abc123", "username": "小爱" } }
```
响应：
```json
{ "type": "auth.register_res", "data": { "code": 0, "msg": "ok", "data": { "userId": 1, "account": "13800138000", "username": "小爱" } } }
```
- `account`：登录账号，必填，唯一（重复回 `409 账号已存在`），**10-11 位纯数字**（如手机号）；格式不满足回 `400 账号须为10-11位数字`，客户端应在输入侧做同样限制（只允许数字、长度 10-11）
- `password`：**6-32 位，仅字母/数字，且同时含字母与数字**；不满足回 `400 参数错误`，客户端应提示密码格式
- `username`：昵称（可选，空则=account），长度 **< 32**

### 5.2 登录 `auth.login`

请求：
```json
{ "type": "auth.login", "seq": 2, "data": { "account": "13800138000", "password": "abc123" } }
```
响应（成功）：
```json
{ "type": "auth.login_res", "data": { "code": 0, "msg": "ok", "data": { "userId": 1, "account": "13800138000", "username": "小爱", "token": "a1b2c3..." } } }
```
响应（失败）：
```json
{ "type": "auth.login_res", "data": { "code": 403, "msg": "账号或密码错误", "data": {} } }
```
> `token` 由客户端保存，**用于断线免密重连**（见 §5.5、§8.3）；不参与除 `auth.token` 外的任何指令。
> 登录不校验账号格式：账号不存在与密码错误统一回 `403 账号或密码错误`（不泄露账号存在性）。

### 5.3 登出 `auth.logout`

请求：
```json
{ "type": "auth.logout", "seq": 3, "data": {} }
```
响应：
```json
{ "type": "auth.logout_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```

### 5.4 发送单聊消息 `msg.send`

请求（`to` 为目标**登录账号 `account`**，v1.7 起消息定位一律用 account）：
```json
{ "type": "msg.send", "seq": 4, "data": { "to": "13900139000", "content": "你好", "clientMsgId": "8f14e45f-0001", "chatType": "single" } }
```
响应（服务端已接收）：
```json
{ "type": "msg.ack", "data": { "code": 0, "msg": "ok", "clientMsgId": "8f14e45f-0001" } }
```
- `to`：目标**登录账号 `account`**（10-11 位纯数字，传数字或字符串均可）；**`to == 自己 account` 表示"自己对话框"**（v1.8，已删除 `to:0` 特例）——发给自己的消息**落库**，服务器**只回 `msg.ack`，不投递 `msg.recv`**，由客户端在"自己对话框"里本地显示这条消息（避免回显重复），且可用 `chat.history` 拉回（非空）。**兼容旧协议**：`to` 传数字 `userId` 仍有效（服务器按 userId 校验）
- **发送权限（v1.8）**：`to` 为**自己**、**自己的联系人**、或**目标开启临时对话**（`allowTempChat=1`）时可发送；否则 `msg.ack` 回 **`403 对方未开启临时对话，请先添加联系人`** 且**不落库**（非联系人须先发起 `contact.request` 成为联系人，见 §5.11）
- `content`：消息文本，**上限 64KB**
- `clientMsgId`：发送端生成（建议 UUID，长度上限 64 字节，超限回 `400 参数错误`），用于与 `msg.ack` 对齐；**服务端按发送方 + `clientMsgId` 做幂等去重**——5 分钟窗口内重发相同 id，服务端只投递一次但正常回 `msg.ack(code=0)`（客户端网络重试安全）；**被限流拒绝（429）的 `clientMsgId` 不会污染幂等记录**，窗口内重试仍返回 429 而非误报成功
- `chatType`：可选，缺省 `"single"`；**`"group"` 时 `to` 为数字 `groupId`**（整数必填，见 §5.18 群聊）：群不存在回 `404 群不存在`，非群成员回 `403 非群成员`，`to` 缺失/非整数回 `400 参数错误`
- **群发送（v1.9）**：请求示例 `{ "type": "msg.send", "seq": 16, "data": { "to": 1, "content": "大家好", "clientMsgId": "8f14e45f-0002", "chatType": "group" } }`；内容规则与单聊一致（空消息 `400 参数错误`，超 64KB `413`）；群成员在线收 `msg.recv`（含 `groupId`/`groupName`，发送者不回显），离线登录后经 `msg.offline` 补发（同样不含自己发的消息）；群消息**不做已读**；限流/幂等与单聊同一套规则
- `msg.ack` 只表示服务端已接收，不代表对方已读
- 发送限流：携带 `clientMsgId` 每用户 10 秒最多 30 条；空 `clientMsgId` 10 秒最多 5 条；超限 `msg.ack` 回 `429 消息发送过于频繁`，客户端应提示发送过频

### 5.5 免密重连 `auth.token`

请求：
```json
{ "type": "auth.token", "seq": 5, "data": { "token": "a1b2c3..." } }
```
响应：
```json
{ "type": "auth.token_res", "data": { "code": 0, "msg": "ok", "data": { "userId": 1, "username": "小爱", "token": "a1b2c3..." } } }
```
- `token` 为上次 `auth.login_res` / `auth.token_res` 返回的 token，有效期 7 天
- 成功即恢复登录态，随后可能收到 `msg.offline` 离线补发
- 失败回 `code == 401`（token 无效/过期），此时应改用 `auth.login` 重新登录

### 5.6 新建对话 `chat.create`

> 客户端发起对话前，用目标**登录账号 `account`** 校验对方是否存在，并拿到其 `account` 与**昵称**用于展示。

请求：
```json
{ "type": "chat.create", "seq": 6, "data": { "account": "13900139000" } }
```
响应（成功）：
```json
{ "type": "chat.create_res", "data": { "code": 0, "msg": "ok", "data": { "userId": 2, "account": "13900139000", "username": "小波" } } }
```
响应（目标不存在）：
```json
{ "type": "chat.create_res", "data": { "code": 404, "msg": "目标用户不存在", "data": {} } }
```
- `data.account`：目标**登录账号**（必填，**10-11 位纯数字**；格式不满足回 `400 账号须为10-11位数字`），服务器按此查找用户
- `data.username` 为目标**昵称**，客户端可直接用于会话标题/列表展示
- 未登录回 `401 未登录`；`account` 格式不合规回 `400 账号须为10-11位数字`；**合规但不存在**回 `404 目标用户不存在`
- **自己对话框**：`account` 填**自己的登录账号** → 返回自己的 `userId`、`account`、昵称

### 5.7 离线消息确认 `msg.offline_ack`（v1.5）

收到 `msg.offline` 后，客户端应确认已收到（防重复补发）：
```json
{ "type": "msg.offline_ack", "data": { "messageIds": ["1720000000-12", "1720000000-13"] } }
```
- `messageIds`：`msg.offline.msgs[]` 中各消息的 `messageId`；单次上限 1000（超限回 `400 确认数量超限`，请分批确认）
- 服务端将已确认消息标记为已投递，之后不再补发；**未确认的消息保留**，重登仍会推送
- 单次登录补发上限 100 条单聊离线消息（群离线每群 100 条）；超量时确认后重登继续补发，渐进收齐
- 客户端建议用 `messageId` 本地去重，避免断线重试造成重复展示

### 5.8 单聊历史 `chat.history`（v1.5）

```json
{ "type": "chat.history", "data": { "to": "13900139000", "limit": 50 } }
```
- `to`：目标**登录账号 `account`**（v1.7）；**`to == 自己 account` 表示"自己对话框"**（v1.8，已删除 `to:0` 特例），返回自己对话框的消息（自 v1.8 起自己对话框消息**落库、历史可查非空**）；`limit` 可选，默认 50，上限 200
- 回 `chat.history_res`，`data.msgs[]` 元素与 `msg.recv` 相同，**按时间倒序（最新在前）**
- 客户端可在打开会话时拉取历史，补齐消息列表

### 5.9 已读回执 `msg.read`（v1.5）

```json
{ "type": "msg.read", "data": { "from": "13900139000", "ts": 1720000000 } }
```
- `from`：对方**登录账号 `account`**（v1.7）；**`from == 自己 account`**（v1.8，已删除 `0` 特例）直接成功（无对端消息）；`ts` 可选，默认当前时间
- 把与该用户截至 `ts` 的未读消息标记为已读；回 `msg.read_res`
- 可用于清除会话未读角标（服务端记录已读状态）

### 5.10 搜索用户 `user.search`（v1.8）

按账号/昵称模糊搜索用户（受目标 `searchable` 控制，不含自己）：

```json
{ "type": "user.search", "seq": 7, "data": { "keyword": "138", "limit": 20 } }
```
响应（成功）：
```json
{ "type": "user.search_res", "data": { "code": 0, "msg": "ok", "data": { "users": [ { "account": "13800138000", "username": "小爱", "isContact": false } ] } } }
```
- `keyword`：必填（账号或昵称的模糊关键字）；空回 `400 参数错误`
- `limit`：可选，默认 20，上限 50
- `isContact`：该用户是否已是我的联系人（`true` 时客户端可直接进对话，无需再发请求）
- 未登录回 `401 未登录`

### 5.11 发送好友请求 `contact.request`（v1.8）

```json
{ "type": "contact.request", "seq": 8, "data": { "account": "13900139000" } }
```
响应（成功）：
```json
{ "type": "contact.request_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- 成功时**对方在线会收到 `contact.request` 推送**（`{from, fromNick, ts}`），离线则登录后补发；对方同意/拒绝见 §5.12/§5.13
- 错误码：`400 不能添加自己为联系人` / `404 目标用户不存在` / `409 对方已是你的联系人` / `409 已存在待处理的请求` / `403 对方未开启添加，请使用其他方式联系`

### 5.12 同意好友请求 `contact.accept`（v1.8）

```json
{ "type": "contact.accept", "seq": 9, "data": { "account": "13800138000" } }
```
响应（成功）：
```json
{ "type": "contact.accept_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- 同意后双方成为联系人；请求方在线会收到 `contact.added`（`{account, username}`）推送
- **互相添加**：若同时存在对方发给你的待处理请求，同意你收到的请求后，该反向请求也一并关闭，双方 `contact.requests` 不再有对方残留
- 错误码：`404 不存在该待处理请求`（需先 `contact.requests` 确认存在）

### 5.13 拒绝好友请求 `contact.reject`（v1.8）

```json
{ "type": "contact.reject", "seq": 10, "data": { "account": "13800138000" } }
```
响应（成功）：
```json
{ "type": "contact.reject_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- 错误码：`404 不存在该待处理请求`

### 5.14 联系人列表 `contact.list`（v1.8）

```json
{ "type": "contact.list", "seq": 11, "data": {} }
```
响应（成功）：
```json
{ "type": "contact.list_res", "data": { "code": 0, "msg": "ok", "data": { "contacts": [ { "account": "13900139000", "username": "小波" } ] } } }
```
- 用于客户端启动时拉取联系人，作为会话列表/发起对话的入口
- **返回恒含自己的条目（排第一位）**：`account=自己的账号、username=自己的昵称`（v1.8.1）；注册时自动建立，登录不重复写入

### 5.15 待处理请求列表 `contact.requests`（v1.8）

```json
{ "type": "contact.requests", "seq": 12, "data": {} }
```
响应（成功）：
```json
{ "type": "contact.requests_res", "data": { "code": 0, "msg": "ok", "data": { "requests": [ { "account": "13800138000", "username": "小爱", "ts": 1720000000 } ] } } }
```
- 客户端可轮询/在收到 `contact.request` 推送后刷新"好友申请"列表

### 5.16 删除联系人 `contact.delete`（v1.8）

```json
{ "type": "contact.delete", "seq": 13, "data": { "account": "13900139000" } }
```
响应（成功）：
```json
{ "type": "contact.delete_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- 删除后双方不再是联系人；对方在线会收到 `contact.deleted`（`{account}`）推送
- 错误码：`400 参数错误`（含不能删除自己）/ `404 对方不是你的联系人`

### 5.17 隐私设置 `settings.get` / `settings.set`（v1.8）

查询：
```json
{ "type": "settings.get", "seq": 14, "data": {} }
```
响应（成功）：
```json
{ "type": "settings.get_res", "data": { "code": 0, "msg": "ok", "data": { "allowTempChat": true, "searchable": true, "allowAdd": true } } }
```

更新（缺省字段不修改）：
```json
{ "type": "settings.set", "seq": 15, "data": { "allowTempChat": false } }
```
响应（成功）：
```json
{ "type": "settings.set_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- `allowTempChat`：是否允许非联系人临时对话；`false` 时非联系人 `msg.send` 回 `403 对方未开启临时对话，请先添加联系人`
- `searchable`：是否可被 `user.search` 搜到；`false` 时其他用户搜索不到自己
- `allowAdd`：是否可被添加为联系人；`false` 时他人 `contact.request` 回 `403 对方未开启添加，请使用其他方式联系`
- 三开关**默认全开**；客户端应在设置页提供三个开关并调用 `settings.set` 同步

### 5.18 建群 `group.create`（v1.9）

```json
{ "type": "group.create", "seq": 16, "data": { "name": "周末徒步群" } }
```
响应（成功）：
```json
{ "type": "group.create_res", "data": { "code": 0, "msg": "ok", "data": { "groupId": 1, "name": "周末徒步群" } } }
```
- `name`：群名，必填，非空且长度 ≤ 32，否则回 `400 参数错误`
- 建群者自动成为群主；`groupId` 为数字自增 id（与 `userId` 命名空间独立），后续群操作均用它定位
- 未登录回 `401 未登录`

### 5.19 群主拉人 `group.invite`（v1.9）

```json
{ "type": "group.invite", "seq": 17, "data": { "groupId": 1, "account": "13900139000" } }
```
响应（成功）：
```json
{ "type": "group.invite_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- 仅群主可拉人；成功时被拉人在线会收到 `group.added`（`{groupId, name, inviter}`）推送，群内其他在线成员会收到 `group.member_changed`（`{groupId, change: "join", account}`）推送
- 错误码：`400 参数错误`（`groupId` 缺失/非整数）/ `404 群不存在` / `404 目标用户不存在` / `403 仅群主可拉人` / `403 群已满`（50 人，含群主）/ `409 对方不是你的联系人`（被拉人须先是邀请人的联系人）/ `409 对方已在群中`

### 5.20 退群 `group.leave`（v1.9）

```json
{ "type": "group.leave", "seq": 18, "data": { "groupId": 1 } }
```
响应（成功）：
```json
{ "type": "group.leave_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- 成功时群内剩余在线成员会收到 `group.member_changed`（`{groupId, change: "leave", account}`）推送
- 错误码：`400 参数错误` / `404 群不存在` / `403 群主不能退群` / `403 非群成员`

### 5.21 我的群列表 `group.list`（v1.9）

```json
{ "type": "group.list", "seq": 19, "data": {} }
```
响应（成功）：
```json
{ "type": "group.list_res", "data": { "code": 0, "msg": "ok", "data": { "groups": [ { "groupId": 1, "name": "周末徒步群", "memberCount": 3, "isOwner": true } ] } } }
```
- 用于客户端启动时拉取群，作为群会话列表入口；`isOwner` 表示我是否为群主
- 被拉入群但离线时，登录后用 `group.list` 发现新群（配合 `msg.offline` 补群消息，自洽）

### 5.22 群成员列表 `group.members`（v1.9）

```json
{ "type": "group.members", "seq": 20, "data": { "groupId": 1 } }
```
响应（成功）：
```json
{ "type": "group.members_res", "data": { "code": 0, "msg": "ok", "data": { "members": [ { "account": "13800138000", "username": "小爱", "joinedTs": 1720000000, "isOwner": true } ] } } }
```
- 须为群成员，否则回 `403 非群成员`；`400 参数错误`（`groupId` 缺失/非整数）

### 5.23 群历史 `group.history`（v1.9）

```json
{ "type": "group.history", "seq": 21, "data": { "groupId": 1, "limit": 50 } }
```
响应（成功）：
```json
{ "type": "group.history_res", "data": { "code": 0, "msg": "ok", "data": { "msgs": [ { "from": "13800138000", "fromNick": "小爱", "groupId": 1, "groupName": "周末徒步群", "content": "大家好", "ts": 1720000000, "clientMsgId": "...", "messageId": "...", "chatType": "group" } ] } } }
```
- `limit` 可选，默认 50，上限 200；`msgs[]` **按时间倒序（最新在前）**，元素含 `groupId`/`groupName`，`chatType` 为 `"group"`
- 仅返回加群后、退群前的消息；非（曾）成员回 `403 非群成员`
- 群消息**不做已读**（无群已读回执）；`chat.history` 单聊视图已加群隔离，群消息不会混入

## 6. 服务端 → 客户端事件

### 6.1 收到新消息 `msg.recv`（单聊）

```json
{ "type": "msg.recv", "data": { "from": "13800138000", "fromNick": "小爱", "content": "你好", "ts": 1720000000, "clientMsgId": "8f14e45f-0001", "messageId": "1720000000-12", "chatType": "single" } }
```
- `from`：发送方**登录账号 `account`**（v1.7，用于定位会话/好友）
- `fromNick`：发送方**昵称**，直接用于气泡展示，无需再查

### 6.2 离线消息补发 `msg.offline`（登录成功后可能收到）

```json
{ "type": "msg.offline", "data": { "msgs": [ { "from": "13800138000", "fromNick": "小爱", "content": "在吗", "ts": 1720000000, "clientMsgId": "8f14e45f-0001", "messageId": "1720000000-12", "chatType": "single" } ] } }
```
- 登录成功后才可能收到；`msgs[]` 元素结构与 `msg.recv` 相同（含 `clientMsgId`、`chatType`、`fromNick`、`messageId`），按单聊处理即可；`messageId` 为服务器生成的全局唯一 ID（可选字段，客户端忽略或用于本地去重）
- **收到后须发 `msg.offline_ack` 确认**（见 §5.7）：确认前消息保留，重登仍会补发；确认后不再补发
- **补发为非破坏性**：补发过程中断连后重登，服务器会再次推送未确认的离线消息；客户端可用 `messageId` 去重，避免重复展示

### 6.3 消息回执 `msg.ack`

见 §5.4 响应。

### 6.4 错误事件 `error`

```json
{ "type": "error", "data": { "code": 401, "msg": "未登录" } }
```
- 结构为简式 `{code, msg}`（与业务响应的 `{code,msg,data}` 不同，解析时注意区分）
- `msg` 为**中文错误原因**（如 `账号或密码错误`），客户端应依据 `code` 判断错误类型

### 6.5 离线确认回执 `msg.offline_ack_res`

```json
{ "type": "msg.offline_ack_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- `msg.offline_ack` 的响应；`code != 0` 表示确认失败（可稍后重试）

### 6.6 历史查询回执 `chat.history_res`

```json
{ "type": "chat.history_res", "data": { "code": 0, "msg": "ok", "data": { "msgs": [ { "from": "13800138000", "fromNick": "小爱", "content": "你好", "ts": 1720000000, "clientMsgId": "...", "messageId": "...", "chatType": "single" } ] } } }
```
- `data.msgs[]` 元素结构与 `msg.recv` 相同，`from` 为发送方 `account`，**按时间倒序（最新在前）**，最多 200 条
- `code=404` 表示目标用户不存在；`to == 自己 account` 返回自己对话框消息（v1.8 起落库，非空历史）

### 6.7 已读回执响应 `msg.read_res`

```json
{ "type": "msg.read_res", "data": { "code": 0, "msg": "ok", "data": {} } }
```
- `msg.read` 的响应；`code=400` 表示 `from` 缺失/非法

### 6.8 好友请求推送 `contact.request`（v1.8）

```json
{ "type": "contact.request", "data": { "from": "13800138000", "fromNick": "小爱", "ts": 1720000000 } }
```
- 有人向我发起好友请求时推送；**在线直接推送，离线登录后补发**（也可用 `contact.requests` 主动拉取）
- `from` 为发起方 `account`，`fromNick` 为发起方昵称，`ts` 为发起时间
- 客户端应展示"好友申请"并让用户选择 §5.12 同意 / §5.13 拒绝

### 6.9 成为联系人推送 `contact.added`（v1.8）

```json
{ "type": "contact.added", "data": { "account": "13900139000", "username": "小波" } }
```
- 我发起的请求被对方同意后，向我推送；`account`/`username` 为**接受方**（对方）信息，客户端可将对方加入联系人列表

### 6.10 被删除联系人推送 `contact.deleted`（v1.8）

```json
{ "type": "contact.deleted", "data": { "account": "13900139000" } }
```
- 对方把我从联系人中删除时向我推送；`account` 为**删除方**账号，客户端应从联系人列表移除该用户

## 7. 错误码速查

| code | 名称 | 客户端处理建议 |
|---|---|---|
| 0 | OK | 成功 |
| 400 | BAD_PACKET | 请求格式错误，检查字段（含不能添加自己） |
| 401 | NOT_LOGIN | 未登录就发消息，请先登录 |
| 403 | WRONG_PASSWORD | 登录失败；**对方未开启临时对话/未开启添加**；**群场景（非群成员 / 仅群主可拉人 / 群主不能退群 / 群已满）** |
| 404 | USER_NOT_FOUND | 目标用户不存在 / 不存在该待处理请求 / 对方不是你的联系人 / **群不存在** |
| 409 | USER_EXISTS | 注册时登录账号已存在；**对方已是联系人 / 已存在待处理的请求**；**拉群时对方不是你的联系人 / 对方已在群中** |
| 413 | MSG_TOO_LONG | 消息超过 64KB |
| 429 | TOO_MANY_ATTEMPTS | 登录失败次数过多被临时锁定，稍后再试 |
| 429 | TOO_MANY_MESSAGES | 消息发送过频被限流，稍后再试 |
| 500 | INTERNAL | 服务端内部错误（如消息存储失败），提示稍后重试 |

> `msg` 字段为**中文错误原因**，客户端应依据 `code` 判断错误类型，`msg` 仅用于展示。各 `code` 对应的 `msg`：
> `400`→无效的封包 / 封包格式错误 / 封包字段错误 / 参数错误 / 未知的指令类型 / 不能添加自己为联系人；
> `401`→未登录 / 登录凭证无效或已过期；
> `403`→账号或密码错误 / 对方未开启临时对话，请先添加联系人 / 对方未开启添加，请使用其他方式联系 / 非群成员 / 仅群主可拉人 / 群主不能退群 / 群已满；`404`→目标用户不存在 / 不存在该待处理请求 / 对方不是你的联系人 / 群不存在；
> `409`→账号已存在 / 对方已是你的联系人 / 已存在待处理的请求 / 对方不是你的联系人 / 对方已在群中；`413`→消息内容超长（超过64KB）；
> `429`→登录尝试过于频繁，请稍后再试 / 消息发送过于频繁，请稍后再试；`500`→服务器内部错误

## 8. 客户端实现指引

### 8.1 连接状态机

```
DISCONNECTED --连接--> CONNECTING --握手成功--> CONNECTED(未登录)
CONNECTED(未登录) --auth.login 成功--> CONNECTED(已登录)
CONNECTED(已登录) --连接断开--> DISCONNECTED
CONNECTED(已登录) --auth.logout 成功--> CONNECTED(未登录)
```

### 8.2 消息发送与 ACK 对齐

- 每条 `msg.send` 生成唯一 `clientMsgId`，发送前暂存待确认
- 收到 `msg.ack` 且 `clientMsgId` 匹配 → 标记已送达；超时未回则提示失败（本次无服务端超时重发，由客户端自行处理）
- `clientMsgId` 同时用于对方端去重（若网络抖动重发）

### 8.3 断线重连

1. 连接断开后，等待重连间隔（建议 1~3 秒）
2. 重建 WebSocket 连接
3. 优先发送 `auth.token` 免密恢复登录态（token 有效期内，见 §5.5）
4. 若 `auth.token` 失败（`401`，token 过期/无效）或无可用 token，改用 `auth.login` 重新登录（用 `account`）
5. 恢复登录态后补收 `msg.offline`

### 8.4 心跳

- 无需客户端主动发包：服务端每 30 秒发 `Ping`，WebSocket 库自动回 `Pong`
- 客户端可持续 60 秒不收到任何帧即视为断线（`Ping` 也算），据此触发重连

### 8.5 与用户模型的配合要点

- 登录成功后保存 `account`、`username`（昵称）、`token`；**`userId` 可忽略**（对客户端不透明）
- 发消息：`msg.send.to` 填目标 **`account`**（不是 userId，也不是昵称）
- 收消息：用 `from`（对方 account）作为会话/聊天的键，用 `fromNick` 直接展示对方昵称；若对方改昵称，下次收到消息自动更新
- "自己对话框"：`chat.create` 用**自己的 `account`** 发起（返回自己的信息）；`msg.send` 的 `to` 填**自己的 `account`**（v1.8 已删除 `0` 特例），发送后**不会收到 `msg.recv`**，客户端应在"自己对话框"中本地追加这条消息（`msg.ack` 成功后即可显示）；自己对话框消息**落库**，`chat.history` 可查

### 8.6 搜索 → 添加 → 同意 → 对话流程（v1.8）

新用户通过**搜索**结识并聊天：

1. **搜索**：登录后，发起 `user.search`（关键字为对方账号或昵称）→ `user.search_res` 拿到候选用户 `{account, username, isContact}`
2. **判断**：`isContact == true` 直接进入对话；否则先走好友流程
3. **发送请求**：`contact.request {account}` → `contact.request_res` 成功；若对方 `allowAdd=0` 回 `403 对方未开启添加`，提示无法添加
4. **等待/处理**：对方在线收 `contact.request` 推送（离线则登录后补发，也可用 `contact.requests` 主动刷新）→ 选择 `contact.accept`（同意）或 `contact.reject`（拒绝）
5. **成为联系人**：同意后请求方收 `contact.added` 推送；双方均可 `contact.list` 拉取完整联系人列表
6. **对话**：此时 `msg.send {to: 对方 account}` 必可发送（联系人无需 `allowTempChat`）；若对方关闭临时对话，非联系人在 `contact.request` 成功前 `msg.send` 会回 `403 对方未开启临时对话，请先添加联系人`
7. **删除**：需要时 `contact.delete {account}` 移除联系人，对方收 `contact.deleted` 推送

### 8.7 建群 → 拉人 → 群聊流程（v1.9）

联系人之间发起群聊：

1. **建群**：群主发起 `group.create {name}` → `group.create_res` 拿到 `groupId`
2. **拉人**：群主逐个 `group.invite {groupId, account}`（被拉人须是群主的联系人，否则 `409 对方不是你的联系人`）→ `group.invite_res` 成功；被拉人在线收 `group.added` 推送（`{groupId, name, inviter}`），群内其他在线成员收 `group.member_changed`（`change: "join"`）；被拉人离线时登录后用 `group.list` 发现新群
3. **群聊**：成员 `msg.send {to: groupId, chatType: "group"}` → `msg.ack`；在线成员收 `msg.recv`（含 `groupId`/`groupName`，发送者不回显），离线成员登录后经 `msg.offline` 补发
4. **成员/历史**：打开群会话时 `group.members` 拉成员、`group.history` 拉历史（仅加群后可见）；群消息不做已读
5. **退群**：成员 `group.leave {groupId}` → `group.leave_res`；剩余在线成员收 `group.member_changed`（`change: "leave"`）；群主不能退群（`403 群主不能退群`）

## 9. Qt 与 Web 接入要点对照

| 项目 | Qt（QWebSocket） | Web（浏览器 WebSocket） |
|---|---|---|
| 建立连接 | `socket.open(QUrl("ws://host:9191/"))` | `new WebSocket("ws://host:9191/")` |
| 发送文本 | `socket.sendTextMessage(jsonString)` | `ws.send(jsonString)` |
| 接收文本 | `textMessageReceived` 信号 | `ws.onmessage` |
| 自动回 Pong | QWebSocket 自动处理 | 浏览器自动处理 |
| 心跳感知 | 接收任意帧即存活 | `onmessage` 更新存活时间 |
| JSON 序列化 | `QJsonDocument` | `JSON.parse / JSON.stringify` |

> 双方必须使用完全相同的 `type`、`data` 字段与错误码（§5~§7），即可与服务端互通、互发消息。