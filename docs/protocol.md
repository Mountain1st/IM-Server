# IM 协议文档（v1.8）

> 本协议为服务端、Qt 客户端、Web 客户端三端共用的统一消息协议。
> 传输层为 WebSocket 文本帧（UTF-8 编码的 JSON 字符串）。

**版本变更（v0.1 → v1.8）**
- v1.8.1 **联系人列表恒含"自己"**：`contact.list` 返回中恒有一条 `account=自己账号、username=自己昵称` 的联系人，排第一位；新用户注册时自动建立（写入 contacts 表），存量用户可用 `db/migrate_self_contact.sql` 一次性补齐；登录不重复写入
- v1.8 **联系人体系与隐私设置**：新增 `user.search`（按账号/昵称模糊搜索，受 `searchable` 控制）；`contact.request/accept/reject/list/requests/delete`（好友请求状态机，同意后双方成为联系人；`contact.request` 在线推送、离线登录补发）；`settings.get/set` 三开关（`allowTempChat` 允许非联系人临时对话、`searchable` 可被搜索、`allowAdd` 可被添加）；`msg.send` 权限：自己对话框/联系人/目标开启临时对话才可发送，否则 `403 对方未开启临时对话`；**自己对话框统一为 `to==自己 account`（删除 `to:0` 特例），消息落库、历史可查**
- v1.7 **消息收发目标一律用 `account`（登录账号）**：`msg.send` 的 `to`、`chat.history` 的 `to`、`msg.read` 的 `from` 均改为按登录账号定位目标（兼容旧协议传数字 `userId` 与 `0`=自己对话框）；`msg.recv`/`msg.offline`/`chat.history_res` 的 `from` 改为返回发送方 `account`（**`userId` 对客户端不透明**）；`chat.history` 与 `msg.read` 支持**自己对话框**（`to`/`from` 为 `0` 或自己的 `account`：历史返回空列表、已读直接成功，不再误报 `404 目标用户不存在`）
- v1.6 账号规则收紧：**`account` 必须为 10-11 位纯数字**（如手机号）。`auth.register` 与 `chat.create` 按此校验，不合规返回 `400 账号须为10-11位数字`（如输入 `0` 不再误报 `404 目标用户不存在`）；`auth.login` 不做格式校验（错误统一 403，兼容遗留账号、不泄露账号存在性）
- v1.5 消息持久化与可靠性闭环：**消息落 MySQL**（重启不丢失，离线/历史/已读均持久化）；新增 **`msg.offline_ack`**（客户端确认离线消息已收到，服务端标记为已投递，确认后不再重复补发）；新增 **`chat.history`**（拉取与目标用户的单聊历史，按时间倒序）；新增 **`msg.read`**（已读回执，记录接收方已读状态）
- v1.4 服务器加固：`msg.recv`/`msg.offline` 消息体新增全局 **`messageId`**（可选字段，客户端忽略即可）；**客户端帧强制掩码**、RSV/opcode/长度编码/UTF-8/Close code 严格校验；**握手严格化**（请求行 `GET / HTTP/1.1`、必须带 `Host`、`Sec-WebSocket-Key` 须为合法 Base64、Header 名大小写不敏感）；**离线消息改为非破坏性补发**（补发中断不丢失，重登可再次收到）；**幂等/限流顺序修正**（被限流拒绝的消息重试不再被误判为成功）；新增 **`/health`** HTTP 端点（返回运行指标 JSON）；token 摘要化存储与 256 位熵
- v1.3 用户模型调整：`userId` 为数字唯一标识（消息收发目标，不变）；`auth.register`/`auth.login` 改用 **`account`**（登录账号，唯一）作为凭据；`username` 成为**昵称**（展示名，可重复，缺省与账号一致）；`msg.recv`/`msg.offline` 新增发送方昵称 **`fromNick`**；**所有错误的 `msg` 字段改为中文原因**（`code` 不变）；**注册校验**（密码 6-32 位字母数字组合、昵称 <32、账号唯一）；新增 **`chat.create`** 新建对话指令（按 account 校验目标存在性并返回昵称）；**自己对话框**（`msg.send to=0`）只回 ack 不再投递 `msg.recv`；新增**握手 10 秒超时**；服务器监听升级为 **IPv6 双栈**（`::1` 与 `127.0.0.1` 均可直连，兼容 `localhost` 解析为 IPv6 的客户端）
- v1.2 新增 `msg.send` 限流：携带 `clientMsgId` 每用户 10 秒最多 30 条；空 `clientMsgId` 每用户 10 秒最多 5 条，超限回 `429 TOO_MANY_MESSAGES`
- v1.2 服务端握手可选校验 `Origin`/`Host`（防跨站 WebSocket 连接），由启动参数配置允许列表，缺省不校验
- v1.1 新增 `auth.token` 指令：断线重连可用上次登录的 token 免密恢复登录态
- v1.1 明确 `msg.send` 幂等语义：窗口内重发相同 `clientMsgId`，服务端只投递一次并正常回 `msg.ack`
- v0.1 → v1.0 内容：
  - `msg.send` / `msg.recv` 的 data 增加可选字段 `chatType`（本次恒为 `"single"`，群聊时扩展为 `"group"`）
  - 明确错误事件与业务响应结构差异：`error` 为简式 `{code,msg}`；业务响应为 `{code,msg,data}`
  - 新增 §7 连接生命周期

---

## 1. 用户模型

| 属性 | 类型 | 说明 |
|---|---|---|
| `userId` | number | **唯一标识（服务器内部）**：系统分配的数字 id，**对客户端不透明**；不再作为任何指令/事件字段，仅由服务器内部使用 |
| `account` | string | **登录账号**：10-11 位纯数字（如手机号），全局唯一，登录凭据（`auth.register` / `auth.login` 使用），同时是**消息收发定位目标**（`msg.send.to`、`chat.history.to`、`msg.read.from`、`msg.recv.from` 等）与**联系人/搜索/添加的定位目标**（`user.search`、`contact.*` 均用 `account` 定位对方） |
| `username` | string | **昵称**：展示名，可重复；注册时可选，缺省与 `account` 相同；可在响应与 `fromNick` 中获取 |

客户端注意：**登录用 `account`，消息收发定位用 `account`，界面展示用 `username`（昵称）**；`userId` 对客户端不透明，可忽略。

## 2. 传输与环境

- 底层：WebSocket（RFC 6455），服务端默认监听端口 `9191`，路径为 `/`（即 `ws://host:9191/`），后续经 Nginx 提供 `wss://`
- 帧类型：仅使用文本帧（`opcode=0x1`），消息体为单个 JSON 对象（UTF-8）；**客户端发送的帧必须掩码（MASK）**，服务器拒绝未掩码帧；服务器响应帧不掩码
- 握手严格化（v1.4）：请求行必须为 `GET / HTTP/1.1`；必须携带 `Host` 头；`Sec-WebSocket-Key` 必须为合法 Base64（16 字节）；`Upgrade`/`Connection`/`Sec-WebSocket-Version: 13` 校验；Header 名大小写不敏感；不满足的握手被断开
- 心跳：服务端每 30 秒发 `Ping` 帧，客户端须自动回 `Pong`（WebSocket 协议层，不占用业务指令）；连续 60 秒无任何数据视为死连接并断开
- **握手超时**：连接建立后须在 **10 秒内**完成 WebSocket 握手，否则服务器断开（防慢速连接占槽）
- **健康检查**：`GET http://host:9191/health` 返回运行指标 JSON（非 WebSocket 请求），字段：
  ```json
  { "status": "ok",
    "activeConnections": 0, "connectionsAccepted": 0, "connectionsClosed": 0,
    "handshakeFailures": 0, "protocolErrors": 0, "authFailures": 0,
    "rateLimited": 0, "messagesDelivered": 0, "messagesOffline": 0,
    "writeOverflows": 0 }
  ```
  字段含义：当前连接数 / 累计接受 / 累计关闭 / 握手失败 / 协议错误 / 认证失败 / 限流拒绝 / 在线投递 / 离线入队 / 写缓冲超限断开。可用于探活与监控。
- 编码约定：所有时间字段为 Unix 时间戳（秒）
- 握手校验：服务端可通过启动参数配置允许的 `Origin`/`Host` 列表（条目如 `http://localhost:8080` 或裸 `host[:port]`），仅当配置了允许列表时才校验（浏览器 WebSocket 会自动携带 `Origin`）；未配置则接受任意来源连接

## 3. 通用封包（信封）格式

所有客户端 → 服务端指令、服务端 → 客户端事件，均使用如下信封：

```json
{
  "type": "msg.send",
  "seq": 123,
  "ts": 1720000000,
  "data": { }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `type` | string | 是 | 指令/事件名，见 §4、§5 |
| `seq` | number | 否 | 客户端自增序号（建议递增），用于消息与 ACK 对齐 |
| `ts` | number | 否 | 发送方 Unix 时间戳（秒） |
| `data` | object | 是 | 各指令/事件的业务参数，见各节 |

服务端在收到任何指令后都会回包。若封包非法（非 JSON、缺 `type`、`data` 非对象），服务端回错误事件（简式结构）：

```json
{ "type": "error", "data": { "code": 400, "msg": "无效的封包" } }
```

> 结构约定：**错误事件** `error` 的 `data` 为 `{ code, msg }`；**业务响应事件**（`auth.*_res`、`msg.ack` 等）的 `data` 为 `{ code, msg, data }`。客户端需分别解析这两种结构。

## 4. 指令表（C2S：客户端 → 服务端）

| type | data 参数 | 说明 |
|---|---|---|
| `auth.register` | `{ "account": "...", "password": "...", "username": "昵称(可选)" }` | 注册新账号（account 登录账号，username 昵称缺省=account），成功回 `auth.register_res` |
| `auth.login` | `{ "account": "...", "password": "..." }` | 用登录账号登录，成功回 `auth.login_res`（携带 token） |
| `auth.token` | `{ "token": "..." }` | 免密重连：用上次登录的 token 恢复登录态，成功回 `auth.token_res` |
| `auth.logout` | `{}` | 主动登出，回 `auth.logout_res` |
| `msg.send` | `{ "to": "<account>", "content": "...", "clientMsgId": "uuid", "chatType": "single" }` | 发送单聊消息（`to` 为目标登录账号，v1.7；兼容旧协议传数字 `userId`），回 `msg.ack`；消息持久化到 MySQL |
| `chat.create` | `{ "account": "登录账号" }` | 新建对话：按 account 查找目标用户，存在回其 account 与昵称；不存在回 404 |
| `msg.offline_ack` | `{ "messageIds": ["...", ...] }` | 客户端确认已收到 `msg.offline` 中的消息，服务端标记为已投递（确认后不再补发）；回 `msg.offline_ack_res` |
| `chat.history` | `{ "to": "<account>", "limit": 50(可选) }` | 拉取与目标用户的单聊历史（`to` 为目标登录账号，v1.7；兼容旧协议传数字 `userId`），按 ts 倒序，最新在前，默认 50 条，上限 200；回 `chat.history_res` |
| `msg.read` | `{ "from": "<account>", "ts": 1720000000(可选) }` | 已读回执（`from` 为对方登录账号，v1.7；兼容旧协议传数字 `userId`）：把与 `from` 用户截至 `ts` 的未读消息标记为已读；回 `msg.read_res` |
| `user.search` | `{ "keyword": "...", "limit": 20(可选) }` | 按账号/昵称模糊搜索（v1.8，受目标 `searchable` 控制，排除自己），回 `user.search_res` |
| `contact.request` | `{ "account": "<对方账号>" }` | 发送好友请求（v1.8），回 `contact.request_res`；成功在线推送对方 `contact.request` 事件、离线登录补发 |
| `contact.accept` | `{ "account": "<对方账号>" }` | 同意好友请求（v1.8），双方成为联系人，回 `contact.accept_res`；成功推送对方 `contact.added` 事件；**若对方也向你发过待处理请求（互相添加），该反向请求一并自动关闭** |
| `contact.reject` | `{ "account": "<对方账号>" }` | 拒绝好友请求（v1.8），回 `contact.reject_res` |
| `contact.list` | `{}` | 拉取我的联系人列表（v1.8），回 `contact.list_res`；**恒含一条自己的联系人（account=自己、username=自己昵称，排第一位）**（v1.8.1） |
| `contact.requests` | `{}` | 拉取收到的待处理好友请求（v1.8），回 `contact.requests_res` |
| `contact.delete` | `{ "account": "<对方账号>" }` | 删除联系人（v1.8），回 `contact.delete_res`；成功推送对方 `contact.deleted` 事件 |
| `settings.get` | `{}` | 查询我的隐私设置三开关（v1.8），回 `settings.get_res` |
| `settings.set` | `{ "allowTempChat"?: bool, "searchable"?: bool, "allowAdd"?: bool }` | 更新隐私设置（v1.8，缺省字段不修改），回 `settings.set_res` |
| `heartbeat` | `{}` | 已预留但**暂未实现**：发送将返回 `400 未知的指令类型`；心跳请依赖 WebSocket 层 Ping/Pong |

约束：

- 未登录的会话只允许发送 `auth.register`、`auth.login`、`auth.token`，其余指令返回 `401 未登录`
- `auth.register` / `auth.login` 的 **`account` 为登录账号**（必填，全局唯一，**10-11 位纯数字**；`auth.register` 与 `chat.create` 校验格式，不合规返回 `400 账号须为10-11位数字`，注册冲突返回 `409 账号已存在`；`auth.login` 不校验格式，账号不存在与密码错误统一回 `403`）
- **`auth.register` 的密码规则**：6-32 位，仅字母/数字，且**同时包含字母与数字**；不满足返回 `400 参数错误`
- **`username` 为昵称**（可选，空则缺省=account，长度 **< 32**，可重复）
- `msg.send` 的 `to` 为目标用户**登录账号 `account`**（10-11 位纯数字，传数字或字符串均可），`content` 长度上限 64KB，超长返回 `413 消息内容超长（超过64KB）`；**兼容旧协议**：`to` 传数字 `userId` 仍有效（服务器按 userId 校验，如 `999999` 等非账号格式）
- `msg.send` 的 `to` 支持 **`to == 自己 account`**（**自己对话框**，v1.8 已删除 `to:0` 特例）：发给自己的消息会**落库**，服务器**只回 `msg.ack`，不投递 `msg.recv`**（避免回显重复），之后 `chat.history` 可查（非空），`msg.read` 直接成功；发给别人的消息也不会回显给自己
- `msg.send` 的 `to` 权限校验（v1.8）：`to` 为**自己账号** 或 **自己的联系人** 或 **目标开启临时对话（`allowTempChat=1`）** 时才能发送；否则回 `403 对方未开启临时对话，请先添加联系人` 且**不落库**
- **三开关（v1.8）**：`allowTempChat`（是否允许非联系人给自己发临时对话）、`searchable`（是否可被 `user.search` 搜索到）、`allowAdd`（是否可被添加为联系人）；三者**默认全开**；`allowTempChat=0` 时非联系人发送被 403 拒绝；`searchable=0` 时搜索结果不返回该用户；`allowAdd=0` 时对方 `contact.request` 回 `403 对方未开启添加，请使用其他方式联系`
- `msg.send` 的 `chatType` 可选；缺省按 `"single"` 处理，非 `"single"` 值返回 `400 封包格式错误`
- `msg.send` 的 `clientMsgId` 由发送端生成（建议 UUID），用于与 `msg.ack` 对齐与幂等去重；**同一发送方在 5 分钟窗口内重发相同 `clientMsgId`，服务端只投递一次**，重复请求仍正常回 `msg.ack(code=0)`
- `msg.send` 限流：携带 `clientMsgId` 每用户 10 秒窗口最多 30 条；**空 `clientMsgId` 不参与去重，走更严限制：10 秒窗口最多 5 条**；超限回 `429 消息发送过于频繁`（`msg.ack` 携带该错误码）
- `auth.token` 的 token 由 `auth.login_res` 返回；token 有效期 7 天，失效后需重新 `auth.login`
- `chat.create` 需已登录；`data.account` 为目标**登录账号**（必填，**10-11 位纯数字**，格式不合规返回 `400 账号须为10-11位数字`，如输入 `0`），服务器按账号查找：存在回 `chat.create_res(code=0, data={userId, account, username=昵称})`，不存在回 `404 目标用户不存在`；**`account` 为自身登录账号即自己对话框**（返回自己的 userId/account/昵称）
- `chat.history` 的 `to` 为目标**登录账号 `account`**（兼容旧协议数字 `userId`）；**`to == 自己 account` 为自己对话框**（v1.8，不再支持 `to:0` 特例），返回自己对话框的消息（自 v1.8 起自己对话框消息**落库、历史可查非空**）；`account` 不存在回 `404 目标用户不存在`
- `msg.read` 的 `from` 为对方**登录账号 `account`**（兼容旧协议数字 `userId`）；**`from == 自己 account`**（v1.8，不再支持 `0` 特例）直接回 `code=0`（无对端消息，无未读可标记）

## 5. 事件表（S2C：服务端 → 客户端）

| type | data 参数 | 说明 |
|---|---|---|
| `auth.register_res` | `{ "code": 0, "msg": "ok", "data": { "userId": 1, "account": "...", "username": "昵称" } }` | 注册结果；`code != 0` 见错误码表 |
| `auth.login_res` | `{ "code": 0, "msg": "ok", "data": { "userId": 1, "account": "...", "username": "昵称", "token": "..." } }` | 登录结果；成功时返回用户信息与 token（token 用于断线免密重连） |
| `auth.token_res` | `{ "code": 0, "msg": "ok", "data": { "userId": 1, "username": "昵称", "token": "..." } }` | 免密重连结果；成功时返回用户信息与当前 token |
| `auth.logout_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 登出结果 |
| `msg.recv` | `{ "from": "<account>", "fromNick": "发送方昵称", "content": "...", "ts": 1720000000, "clientMsgId": "...", "messageId": "全局消息ID", "chatType": "single" }` | 收到的新消息（单聊推送）；`from` 为发送方登录账号（v1.7）；`fromNick` 供直接展示；`messageId` 为服务器生成的全局唯一 ID（v1.4 起，客户端可忽略） |
| `msg.ack` | `{ "code": 0, "msg": "ok", "clientMsgId": "..." }` | 服务端已接收消息的回执，`clientMsgId` 对齐发送端 |
| `msg.offline` | `{ "msgs": [ { "from": "<account>", "fromNick": "...", "content": "...", "ts": 1720000000, "clientMsgId": "...", "messageId": "...", "chatType": "single" }, ... ] }` | 登录后补发的离线消息列表；`msgs[]` 元素结构与 `msg.recv` 相同（`from` 为发送方登录账号）；**非破坏性补发**（v1.5：确认前保留，收到后应发 `msg.offline_ack`，确认后不再补发） |
| `chat.create_res` | `{ "code": 0, "msg": "ok", "data": { "userId": 1, "account": "登录账号", "username": "昵称" } }` | 新建对话结果；成功返回目标用户 `userId`、**`account`** 与昵称；`404` 表示账号不存在 |
| `msg.offline_ack_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 离线消息确认结果 |
| `chat.history_res` | `{ "code": 0, "msg": "ok", "data": { "msgs": [ { "from": "<account>", "fromNick": "...", "content": "...", "ts": 1720000000, "clientMsgId": "...", "messageId": "...", "chatType": "single" }, ... ] } }` | 单聊历史；`msgs[]` 元素结构与 `msg.recv` 相同（`from` 为发送方登录账号），**按 ts 倒序（最新在前）** |
| `msg.read_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 已读回执结果 |
| `user.search_res` | `{ "code": 0, "msg": "ok", "data": { "users": [ { "account": "...", "username": "昵称", "isContact": true }, ... ] } }` | 搜索结果；`isContact` 表示目标是否已是我的联系人 |
| `contact.request_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 好友请求结果；`403` 对方未开启添加、`409` 对方已是联系人 / 已存在待处理请求、`400` 不能添加自己、`404` 目标用户不存在 |
| `contact.accept_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 同意好友请求结果；`404` 不存在该待处理请求 |
| `contact.reject_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 拒绝好友请求结果；`404` 不存在该待处理请求 |
| `contact.list_res` | `{ "code": 0, "msg": "ok", "data": { "contacts": [ { "account": "...", "username": "昵称" }, ... ] } }` | 我的联系人列表 |
| `contact.requests_res` | `{ "code": 0, "msg": "ok", "data": { "requests": [ { "account": "...", "username": "昵称", "ts": 1720000000 }, ... ] } }` | 收到的待处理好友请求列表（按 ts 排序） |
| `contact.delete_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 删除联系人结果；`400` 参数错误 / 不能删自己、`404` 对方不是你的联系人 |
| `settings.get_res` | `{ "code": 0, "msg": "ok", "data": { "allowTempChat": true, "searchable": true, "allowAdd": true } }` | 我的隐私设置三开关（bool） |
| `settings.set_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 更新隐私设置结果 |
| `contact.request` | `{ "from": "<发起方账号>", "fromNick": "发起方昵称", "ts": 1720000000 }` | 收到好友请求的推送（在线直接推送；离线登录后经 `pushPendingRequests` 补发） |
| `contact.added` | `{ "account": "<接受方账号>", "username": "接受方昵称" }` | 好友请求被同意，双方已成为联系人（推送给出请求方） |
| `contact.deleted` | `{ "account": "<删除方账号>" }` | 被对方从联系人删除（推送给对方） |
| `error` | `{ "code": 400, "msg": "..." }` | 通用错误事件（简式结构） |

字段细则：

- `msg.recv.chatType` 本次恒为 `"single"`；`msg.offline.msgs[]` 元素结构与 `msg.recv` 相同，同样携带 `chatType`、`clientMsgId`、`fromNick` 与 `messageId`
- **离线补发与确认（v1.5）**：离线消息持久化到 MySQL；登录/重连时 `msg.offline` 推送 `status=pending` 的消息。客户端收到后应发送 `msg.offline_ack { messageIds }` 确认，服务端将已确认消息标记为 **delivered**，此后不再补发；**未确认的消息保留**（补发中断不丢失，重登仍会推送），供客户端重试。建议客户端用 `messageId` 去重，避免重复展示
- **`fromNick` 为发送方昵称**：客户端直接用于气泡展示，无需再按 `from` 发起查询
- 客户端应**忽略信封及 data 中的未知字段**，保证协议向后兼容
- `msg.ack` 仅表示"服务端已接收"，不代表目标已读/已送达

## 6. 错误码表

| code | 名称 | 含义 |
|---|---|---|
| 0 | OK | 成功 |
| 400 | BAD_PACKET | 封包格式非法 |
| 401 | NOT_LOGIN | 未登录 / token 无效或过期 |
| 403 | WRONG_PASSWORD | 账号或密码错误 / **未开启临时对话** / **对方未开启添加** |
| 404 | USER_NOT_FOUND | 目标用户不存在 / **不存在该待处理请求** / **对方不是你的联系人** |
| 409 | USER_EXISTS | 登录账号已存在 / **对方已是联系人** / **已存在待处理的请求** |
| 413 | MSG_TOO_LONG | 消息超长（>64KB） |
| 429 | TOO_MANY_ATTEMPTS | 登录失败次数过多被临时限流 |
| 429 | TOO_MANY_MESSAGES | `msg.send` 发送过频被限流（`msg.ack` 携带） |
| 500 | INTERNAL | 服务器内部错误（**预留**，当前版本服务端未返回过该码） |

> `msg` 字段为**中文错误原因**，`code` 不变，客户端应依据 `code` 判断错误类型，`msg` 仅用于展示。各 `code` 对应的 `msg`：
> `400`→无效的封包 / 封包格式错误 / 封包字段错误 / 参数错误 / 未知的指令类型 / 账号须为10-11位数字 / 不能添加自己为联系人；
> `401`→未登录 / 登录凭证无效或已过期；
> `403`→账号或密码错误 / 对方未开启临时对话，请先添加联系人 / 对方未开启添加，请使用其他方式联系；`404`→目标用户不存在 / 不存在该待处理请求 / 对方不是你的联系人；
> `409`→账号已存在 / 对方已是你的联系人 / 已存在待处理的请求；`413`→消息内容超长（超过64KB）；
> `429`→登录尝试过于频繁，请稍后再试 / 消息发送过于频繁，请稍后再试；`500`→服务器内部错误

## 7. 时序示例

### 7.1 注册 + 登录

```
Client A                           Server
   |  auth.register                 |
   |------------------------------->|
   |  auth.register_res (userId=1)  |
   |<-------------------------------|
   |  auth.login                    |
   |------------------------------->|
   |  auth.login_res (token)        |
   |<-------------------------------|
```

### 7.2 在线单聊

```
Client A (account="alice")    Server          Client B (account="bob")
   |  msg.send (to="bob")       |
   |-------------------------->|
   |  msg.ack (clientMsgId)    |
   |<--------------------------|
   |                           |  msg.recv (from="alice")
   |                           |------------------------------>|
```

### 7.3 离线补发

```
Client A           Server          Client B（离线）
   | msg.send(to="bob") |
   |---------------->|
   | msg.ack         |        消息进入 B 的离线队列
   |<----------------|
   ...
                        Client B 登录
                        msg.offline 推送离线消息
```

### 7.4 心跳（WebSocket 协议层）

```
Server                    Client
   |---- Ping 帧 ---------->|   （每 30 秒一次）
   |<---- Pong 帧 ----------|
 连续 60 秒无任何数据 → 服务端断开连接
```

### 7.5 新建对话

```
Client A                    Server          Client B (account="bob")
   |  chat.create (account="bob")   |
   |------------------------------->|
   |  chat.create_res (account="bob", username=昵称) |
   |<-------------------------------|
   |  发起 msg.send (to="bob") 开始聊天  |
```

## 8. 连接生命周期

1. **连接**：客户端建立 WebSocket 连接（`ws://host:9191/`）
2. **注册**（新用户）：`auth.register`（含 `account`，可选 `username` 昵称）→ 收到 `auth.register_res`，`code == 0` 注册成功，保存 `account`、`username`
3. **登录**：`auth.login`（用 `account`）→ 收到 `auth.login_res`，`code == 0` 后进入已登录态；保存 `account`、`username`（昵称）、`token`（`userId` 对客户端不透明，可忽略）
4. **收发消息**：可发送 `msg.send`（`to` 为对方 `account`），接收 `msg.recv` / `msg.offline` / `msg.ack`；**收到 `msg.offline` 后应发送 `msg.offline_ack` 确认**（否则重登会重复补发同一批离线消息）；可随时用 `chat.history` 拉取历史、`msg.read` 上报已读
5. **断线重连**：连接断开后，客户端可重建 WebSocket 连接并选择以下任一方式恢复登录态：
   - 免密重连：发送 `auth.token` 携带上次登录的 token，成功回 `auth.token_res`（token 未过期时推荐）
   - 重新登录：发送 `auth.login`（token 过期或未保存时）
6. **登出 / 关闭**：主动登出发送 `auth.logout` 并收到 `auth.logout_res`，随后关闭连接；也可直接关闭连接（服务端自动清理会话）

## 9. 三端一致约定

- Qt（QWebSocket）与 Web（浏览器 WebSocket）客户端必须严格使用本协议的 `type`、`data` 字段与错误码，保证互通
- **用户模型约定**：登录、收发消息、历史、已读全部用 `account`；`userId` 对客户端不透明（服务器内部用）；界面展示用 `username`（昵称，来自响应或 `fromNick`）
- `seq` 建议客户端按会话自增；服务端不依赖其顺序，仅用于客户端本地对齐
- `clientMsgId` 由发送端生成（如 UUID），用于去重与 ACK 对齐；同一发送方窗口内重发相同 id 只投递一次
- 客户端应忽略未知字段，以兼容协议后续扩展
- **持久化现状**：用户（users 表）、消息/离线/历史/已读（messages 表）已持久化到 MySQL（重启不丢失）；token/发送限流/幂等去重均可存 Redis（`IM_REDIS_ENABLED=1` 启用，跨重启保留；未启用则内存）；在线状态（Presence）当前仍为内存

## 10. 服务端配置与部署

### 10.1 启动参数
```
imserver [端口] [IO线程数] [工作线程数] [最大连接数] [允许Origin列表]
# 缺省: 9191 / 3 / 5 / 0(不限) / 空(不校验Origin)
```

### 10.2 环境变量（配置注入）

| 变量 | 默认 | 说明 |
|---|---|---|
| `IM_MYSQL_HOST` | `127.0.0.1` | MySQL 主机（Docker 内填服务名 `mysql`） |
| `IM_MYSQL_PORT` | `3306` | MySQL 端口 |
| `IM_MYSQL_USER` / `IM_MYSQL_PASSWORD` | `IMS_jack` / `520010` | MySQL 账号（Docker 内账号为 IMS_jack，见 compose） |
| `IM_MYSQL_DB` | `IMS_reactor_db` | 数据库名 |
| `IM_REDIS_ENABLED` | `0` | `1` 启用 Redis 存储（token/限流/幂等） |
| `IM_REDIS_HOST` | `127.0.0.1` | Redis 主机（Docker 内填 `redis`） |
| `IM_REDIS_PORT` | `6380` | Redis 端口（Docker 容器内部为 6379） |
| `IM_REDIS_PASSWORD` / `IM_REDIS_DB` | 空 / `0` | Redis 认证与库号 |

MySQL 连接失败启动报错退出；`IM_REDIS_ENABLED=1` 且连接失败同样报错退出。

### 10.3 Docker 部署
仓库含 `Dockerfile` 与 `docker-compose.yml`：`mysql:8.4` + `redis:8-alpine` + `imserver` 三服务编排。
```
docker compose up -d --build
```
- imserver 等 MySQL 健康后再启动（`depends_on + service_healthy`）
- 容器内 imserver 通过服务名连接：`IM_MYSQL_HOST=mysql`、`IM_REDIS_HOST=redis`（端口 6379）
- `mysql_data` / `redis_data` 命名卷持久化数据
- `docker compose` 启用 Redis（`IM_REDIS_ENABLED=1`），token 免密重连跨容器重启不失效
