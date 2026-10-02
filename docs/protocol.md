# IM 协议文档（v3.0）

> 本协议为服务端、Qt 客户端、Web 客户端三端共用的统一消息协议。
> 传输层为 WebSocket 文本帧（UTF-8 编码的 JSON 字符串）。

**版本变更（v0.1 → v3.0）**
- v3.0 **媒体系统（对象存储直传）**：图片从「WS base64 内联 + 本地盘 `/uploads/`」升级为「对象存储（MinIO/S3）预签名直传」——客户端先 `media.presign` 拿 PUT 预签名 URL 直传对象存储，`media.commit` 由服务端 HEAD 校验后落 `media` 表，`msg.send(msgType=image, mediaId)` 只携带媒体引用（不再内联 base64，旧 1.5MB 信封上限移除）；查看时 `media.url` 鉴权后签发短期 GET URL。移除本地盘对象键与 nginx `/uploads/` 静态托管。单文件上限 20MB、PUT/GET 预签名分别 15/60 分钟、presign 每用户 30 次/分钟；`msg.recv`/`msg.offline`/历史透传 `mediaId`（fileId 字符串）。见 §4/§5 及 `docs/superpowers/specs/2026-10-01-media-system-design.md`
- v2.4 **拉黑（黑名单）**：新增 `block.add/remove/list`。语义为**双向断联**：A 拉黑 B 期间，双方单聊双向 403 拒绝不落库（文案分方向：拉黑方「你已拉黑对方，请先解除」/被拉黑方「对方已拒收」），双向好友请求拒绝（被拉黑方文案与 `allowAdd` 关相同——**模糊防探测**）；**群聊不受影响**（群治理靠踢人）；**不追溯**——拉黑前已落库消息照常可见、未领取的 pending 离线消息照常补发。拉黑自动解除联系人关系并关闭双向好友请求；解除后恢复通信能力、**不恢复联系人关系**（需重新走好友请求）。拉黑优先级高于临时对话开关与联系人状态。无 S2C 推送（被拉黑方全程无感；操作方多端靠 `block.list` 同步，与 settings 同策略）。拉黑缓存纳入 syncCaches 自愈（第四个）
- v2.3 **停机优雅化 + 架构四项收敛**：**客户端可见**——服务端停机（部署重启/SIGTERM）时在线连接先收到 WS Close(1001 Going Away) 再断开（此前为裸 TCP 断开），停机前工作队列排空（已入队消息任务处理完，落库不丢；排水超 5 秒放弃）；**服务端内部（客户端无感）**——指令路由由 29 层 if-else 改哈希表（O(1) 查表）；联系人/群成员内存缓存随清理线程低频比对库计数、漂移即全量重建（绕过服务端直改库最迟约 30 分钟自愈）；新连接从 `fd % N` 取模改为**最少连接优先**分配 IO 线程（消除负载倾斜）；**MySQL 断线自愈**（v2.3.1，2026-09-29 生产故障根治：闲置超 `wait_timeout` 被服务端踢掉（errno 4031/2006/2013）后自动重建连接重试一次，登录不再持续失败到重启）
- v2.2 **多端一致性**：单聊/群聊的自发消息**回显到发送者其他在线端**（发起连接不回显，多设备会话视图一致）；新增推送 `msg.read_notify`（`{from, readTs}`，一端已读同步其他端，免 ACK、丢失靠补发/历史拉取自愈）
- v2.1 **在线状态（Presence）上 Redis**：`IM_REDIS_ENABLED=1` 时在线状态存 Redis（`p:online:<userId>` 连接计数 + TTL 兜底），未启用则内存；`contact.list` 与 `group.members` 条目新增 `online` 字段（该用户/成员当前是否在线，多端任一在线即 true）；后台新增离线消息 TTL 清理（超 7 天未领取自动删除，`IM_OFFLINE_PURGE_INTERVAL` 可调）
- v2.0 **群管理闭环**：新增 `group.kick/transfer/dissolve/rename/announce`（踢人、转让群主、解散群、改群名、群公告）；新增推送 `group.removed/owner_changed/dissolved/renamed/announced`（踢人复用 `group.member_changed(kick)`）；`group.list` 条目新增 `announcement`/`announceTs`（已解散群不列出）；解散后除 `group.history` 外其余群操作回 `404 群已解散`
- v1.9 **群聊最小闭环**：新增 `group.create/invite/leave/list/members/history`（建群、群主拉人、退群、列表、成员、历史）；`msg.send` 支持 `chatType: "group"`（`to` 为数字 groupId）；群消息单行存储（`to_user=0, group_id=G`），离线按水位线补发；`msg.recv/msg.offline` 群载荷含 `groupId/groupName`；群不做已读；`chat.history`/`msg.read` 单聊视图加群隔离
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
| `groupId` | number | **群标识（命名空间独立）**：建群时分配的数字自增 id，与用户 `userId` 无关；群收发/列表/成员/历史均用 `groupId` 定位，客户端按 `chatType`（`single`/`group`）区分单聊与群聊 |

客户端注意：**登录用 `account`，消息收发定位用 `account`，界面展示用 `username`（昵称）**；`userId` 对客户端不透明，可忽略。**群聊用 `groupId`**：单聊会话键为对方 `account`，群聊会话键为数字 `groupId`，两者命名空间独立、不会冲突。

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
- **时区约定（2026-09-29 统一）**：服务端**日志时间戳固定 UTC+8（北京时间）**，
  不依赖系统/容器时区（部署容器默认 UTC 也不再错位）；MySQL 容器
  `TZ=Asia/Shanghai`（`NOW()`/`CURRENT_TIMESTAMP` 与 TIMESTAMP 列读取
  均按北京时间）；nginx 访问日志同。协议消息 `ts` 为 Unix 秒（无时区），
  由客户端按本地时区显示
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
| `msg.send` | `{ "to": "<account>", "content": "...", "clientMsgId": "uuid", "chatType": "single", "msgType": "text", "mediaId": "<fileId>" }` | 发送单聊消息（`to` 为目标登录账号，v1.7；兼容旧协议传数字 `userId`），回 `msg.ack`；消息持久化到 MySQL；`msgType` 为 `"text"`（默认，`mediaId` 忽略）或 `"image"`（媒体消息，见媒体消息约束，此时 `mediaId` 为已 commit 的媒体 `fileId`） |
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
| `group.create` | `{ "name": "群名" }` | 建群（v1.9，`name` 非空且长度 ≤ 32），回 `group.create_res`（携带 `groupId` 与 `name`） |
| `group.invite` | `{ "groupId": 1, "account": "<对方账号>" }` | 群主拉人（v1.9，被拉人须是邀请人的联系人），回 `group.invite_res`；成功推送被拉人 `group.added`、群内在线成员 `group.member_changed(join)` |
| `group.leave` | `{ "groupId": 1 }` | 退群（v1.9，群主不能退群），回 `group.leave_res`；成功推送剩余在线成员 `group.member_changed(leave)` |
| `group.list` | `{}` | 拉取我的群列表（v1.9），回 `group.list_res` |
| `group.members` | `{ "groupId": 1 }` | 拉取群成员列表（v1.9，须为群成员），回 `group.members_res` |
| `group.history` | `{ "groupId": 1, "limit": 50(可选) }` | 拉取群历史（v1.9，仅加群后、退群前可见，按 ts 倒序，默认 50 条，上限 200），回 `group.history_res` |
| `block.add` | `{ "account": "对方账号" }` | 拉黑（v2.4，双向断联）：自动解除联系人关系并关闭双向好友请求；重复拉黑回 409；拉黑自己回 400。回 `block.add_res` |
| `block.remove` | `{ "account": "对方账号" }` | 解除拉黑：恢复通信能力，**不恢复联系人关系**（需重新走好友请求）；未拉黑回 409。回 `block.remove_res` |
| `block.list` | `{}` | 我的拉黑列表：`{ blocks: [{account, username, ts}] }`，回 `block.list_res` |
| `group.kick` | `{ "groupId": 1, "account": "<对方账号>" }` | 踢人（v2.0，仅群主），回 `group.kick_res`；成功推送被踢者 `group.removed`、其余在线成员 `group.member_changed(kick)` |
| `group.transfer` | `{ "groupId": 1, "account": "<对方账号>" }` | 转让群主（v2.0，仅群主，原群主变为普通成员），回 `group.transfer_res`；成功推送在线成员 `group.owner_changed` |
| `group.dissolve` | `{ "groupId": 1 }` | 解散群（v2.0，仅群主），回 `group.dissolve_res`；成功推送解散前在线成员 `group.dissolved` |
| `group.rename` | `{ "groupId": 1, "name": "新群名" }` | 改群名（v2.0，仅群主，`name` 非空且长度 ≤ 32），回 `group.rename_res`；成功推送在线成员 `group.renamed` |
| `group.announce` | `{ "groupId": 1, "content": "公告正文" }` | 群公告（v2.0，仅群主，`content` ≤ 1024 字节，空=清空），回 `group.announce_res`；成功推送在线成员 `group.announced`（正文走 `group.list` 拉取） |
| `media.presign` | `{ "fileName": "a.png", "mimeType": "image/png", "size": 102400, "kind": "image" }` | 申请媒体上传（v3.0，须登录）：服务端校验 `kind`（V1 仅 `"image"`）/`mimeType` 白名单/`size`（≤20MB）/文件名后，生成 `fileId` 并落 `media` 表 PENDING 行，回 `media.presign_res`（携带 PUT 预签名 URL）；**每用户 30 次/分钟限流** |
| `media.commit` | `{ "fileId": "<32位hex>", "size": 102400, "width": 800, "height": 600, "duration": 0 }` | 提交媒体（v3.0，须登录，仅上传者可提交）：服务端对对象存储 `HEAD` 校验对象存在且**实际大小与声明 `size` 一致**（以 HEAD 为准，防超传绕过），`width/height/duration` 为客户端上报元数据（服务端不生成缩略图）；成功 PENDING→COMMITTED，回 `media.commit_res`；重复提交幂等 |
| `media.url` | `{ "fileId": "<32位hex>" }` 或 `{ "fileIds": ["...", ...] }` | 取媒体访问 URL（v3.0，须登录且**须为消息相关方**）：鉴权通过后签发短期 GET 预签名 URL，回 `media.url_res`（`urls` 为 `{fileId: {url, expiresIn}}` 映射）；单值 `fileId` 与批量 `fileIds`（≤100）均支持；越权回 403 |

心跳不在业务指令层：保活由 WebSocket 协议层 Ping/Pong 承担（服务器每 30 秒发 Ping，60 秒无响应判僵尸断开），客户端无需发送心跳指令。

约束：

- 未登录的会话只允许发送 `auth.register`、`auth.login`、`auth.token`，其余指令返回 `401 未登录`
- `auth.register` / `auth.login` 的 **`account` 为登录账号**（必填，全局唯一，**10-11 位纯数字**；`auth.register` 与 `chat.create` 校验格式，不合规返回 `400 账号须为10-11位数字`，注册冲突返回 `409 账号已存在`；`auth.login` 不校验格式，账号不存在与密码错误统一回 `403`）
- **`auth.register` 的密码规则**：6-32 位，仅字母/数字，且**同时包含字母与数字**；不满足返回 `400 参数错误`
- **`username` 为昵称**（可选，空则缺省=account，长度 **< 32**，可重复）
- `msg.send` 的 `to` 为目标用户**登录账号 `account`**（10-11 位纯数字，传数字或字符串均可），`content` 长度上限 64KB，超长返回 `413 消息内容超长（超过64KB）`；**兼容旧协议**：`to` 传数字 `userId` 仍有效（服务器按 userId 校验，如 `999999` 等非账号格式）
- `msg.send` 的 `to` 支持 **`to == 自己 account`**（**自己对话框**，v1.8 已删除 `to:0` 特例）：发给自己的消息会**落库**，服务器**只回 `msg.ack`，不投递 `msg.recv`**（避免回显重复），之后 `chat.history` 可查（非空），`msg.read` 直接成功；发给别人的消息也不会回显给自己
- `msg.send` 的 `to` 权限校验（v1.8）：`to` 为**自己账号** 或 **自己的联系人** 或 **目标开启临时对话（`allowTempChat=1`）** 时才能发送；否则回 `403 对方未开启临时对话，请先添加联系人` 且**不落库**
- **三开关（v1.8）**：`allowTempChat`（是否允许非联系人给自己发临时对话）、`searchable`（是否可被 `user.search` 搜索到）、`allowAdd`（是否可被添加为联系人）；三者**默认全开**；`allowTempChat=0` 时非联系人发送被 403 拒绝；`searchable=0` 时搜索结果不返回该用户；`allowAdd=0` 时对方 `contact.request` 回 `403 对方未开启添加，请使用其他方式联系`
- `msg.send` 的 `chatType` 可选，缺省 `"single"`；**`single` 时 `to` 为目标登录账号 `account`**（单聊权限与落库规则见上）；**`group` 时 `to` 为数字 `groupId`（整数必填）**：群不存在回 `404 群不存在`，非群成员回 `403 非群成员`，`to` 缺失/非整数回 `400 参数错误`；其他 `chatType` 值回 `400 封包格式错误`；**群发送同样校验内容**：空消息回 `400 参数错误`，超 64KB 回 `413 消息内容超长（超过64KB）`（媒体消息 `content` 为 fileId 短串，不适用文本长度上限）
- **媒体消息（`msgType: "image"`，v3.0，单聊/群聊同规则）**：`msgType` 仅 `"text"`（默认，可缺省）/`"image"`，其他值回 `400 参数错误`；`msg.ack` 不带 `msgType`。**不再有 base64 内联路径**——`image` 时 `msg.send` 的 `data.mediaId` 为**已 commit 的媒体 `fileId`（32 位 hex）**，服务端校验该媒体存在、属当前用户且状态为 COMMITTED（缺 `mediaId`/非串/空回 `400`，不存在回 `404 媒体不存在`，非本人回 `403 无权操作该媒体`，未提交回 `400 媒体未提交`），落库仅存 `media.id`（`messages.media_id`），`content` 存 `fileId` 供透传；`msg.recv`/`msg.offline`/`chat.history_res`/`group.history_res` 元素透传 `mediaId`（fileId 字符串，非媒体消息为空串）。**客户端渲染**：收到 `msgType=image` 后调用 `media.url`（携带该 `mediaId`）获取短期 GET URL 再加载图片，URL 过期后重新获取；客户端应以 `msgType` 为准渲染，不得嗅探 `content`。**服务端顺序**：媒体消息先校验媒体归属与状态、再做目标/发送限流/权限预检（码与文本路径一致），失败不落库。
- **媒体访问权限（v3.0）**：`media.url` 仅向**消息相关方**签发 URL——服务端查 `messages.media_id` 是否存在当前用户可达的引用消息（单聊 `from`/`to` 为该用户，群聊该用户为群成员），命中才回 `url`，否则回 `403 无权访问该媒体`（**越权 403**）；查询失败按拒绝（fail-closed）。V1 为保守放行（存在任一可达引用即允许），群聊精确可见窗口（入群/退群时间）留待 V2。仅 COMMITTED 媒体可签发 URL，PENDING/GC 记录回 `404 媒体不存在`。
- **媒体上限与清理（v3.0）**：单文件上限 **20MB**；PUT 预签名有效期 **15 分钟**、GET 预签名 **60 分钟**；`media.presign` 每用户 **30 次/分钟**（超限回 `429`）；未 commit 的 PENDING 记录 **24 小时**后由后台 GC 回收（删对象 + 标记）。`media` 表元数据含 `object_key/file_name/mime_type/file_size/width/height/duration/status`，`status` 取 `0=PENDING/1=UPLOADED/2=COMMITTED/3=GC`。**服务端不生成缩略图**（`width/height/duration` 由客户端 commit 时上报）。
- **群聊约束（v1.9）**：`group.create` 的 `name` 非空且长度 ≤ 32，否则 `400 参数错误`；`group.invite` 仅群主可拉人（非群主 `403 仅群主可拉人`），被拉人须是邀请人的联系人（否则 `409 对方不是你的联系人`），已在群 `409 对方已在群中`，群满（50 人，含群主）`403 群已满`，群不存在 / 目标不存在各回 `404 群不存在` / `404 目标用户不存在`，`groupId` 缺失或非整数 `400 参数错误`；`group.leave` 群主不能退群（`403 群主不能退群`），非成员退群 `403 非群成员`，群不存在 `404 群不存在`；`group.members` / `group.history` 非（曾）成员 `403 非群成员`；群消息**不做已读**（无群已读回执，`msg.read` 仅限单聊视图）
- **群管理（v2.0）**：`group.kick/transfer/dissolve/rename/announce` 仅群主可操作（非群主 `403 仅群主可操作`）；`groupId` 缺失/非整数 `400 参数错误`，群不存在 `404 群不存在`；已解散群操作（除 `group.history`）回 `404 群已解散`；`group.kick` 不能踢群主本人（`409 不能踢群主`）、目标不存在 `404 目标用户不存在`、非成员 `409 目标非群成员`；`group.transfer` 只能转给现群成员（`409 不能转让给自己` / `409 目标非群成员`），原群主变为普通成员；`group.rename` 的 `name` 非空且长度 ≤ 32（同建群，否则 `400 参数错误`）；`group.announce` 的 `content` ≤ 1024 字节（超限 `413 消息内容超长`），空=清空；被踢可被重新拉入，重入后仅看新窗口历史（与退群一致）
- **群存储与补发（v1.9）**：群消息单行存储（`to_user=0, group_id=G`）；在线成员直接收 `msg.recv`（发送者不回显），离线成员按水位线在登录/重连时经 `msg.offline` 合并补发（补发同样不含发送者自己发的消息）；`chat.history` / `msg.read` 单聊视图均加群隔离（`group_id=0`），群消息不混入单聊
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
| `msg.recv` | `{ "from": "<account>", "fromNick": "发送方昵称", "content": "...", "ts": 1720000000, "clientMsgId": "...", "messageId": "全局消息ID", "chatType": "single", "mediaId": "<fileId或空串>" }` | 收到的新消息（单聊推送）；`from` 为发送方登录账号（v1.7）；`fromNick` 供直接展示；`messageId` 为服务器生成的全局唯一 ID（v1.4 起，客户端可忽略）；**媒体消息（v3.0）`mediaId` 为 fileId 字符串，非媒体为空串**（客户端据此调 `media.url` 取图）；**群聊推送结构相同，另含 `groupId`（数字群标识）/`groupName` 且 `chatType` 为 `"group"`（v1.9）**；**v2.2 多端一致：发送者自己的其他在线端也会收到该消息（发起连接不回显），多设备会话视图一致**。注意边界：**仅覆盖在线端**——某端离线期间本账号其他端发的消息不会经 `msg.offline` 补发给该端（消息已落库），客户端重登后应拉 `chat.history` / `group.history` 对齐视图 |
| `msg.read_notify` | `{ "from": "<account>", "readTs": 1720000000 }` | 已读同步（v2.2）：该用户某一端调用 `msg.read` 后，其**其他在线端**收到此推送（`from` 为会话对方账号），客户端应将对应会话未读清零；免 ACK，且仅推在线端（离线端重登不补，靠 `chat.history` 拉取自愈） |
| `msg.ack` | `{ "code": 0, "msg": "ok", "clientMsgId": "..." }` | 服务端已接收消息的回执，`clientMsgId` 对齐发送端 |
| `msg.offline` | `{ "msgs": [ { "from": "<account>", "fromNick": "...", "content": "...", "ts": 1720000000, "clientMsgId": "...", "messageId": "...", "chatType": "single", "mediaId": "<fileId或空串>" }, ... ] }` | 登录后补发的离线消息列表；`msgs[]` 元素结构与 `msg.recv` 相同（`from` 为发送方登录账号；**媒体消息 `mediaId` 为 fileId 字符串（v3.0）；群消息元素另含 `groupId`/`groupName` 且 `chatType` 为 `"group"`（v1.9，单聊与群按水位线合并补发）**）；**非破坏性补发**（v1.5：确认前保留，收到后应发 `msg.offline_ack`，确认后不再补发） |
| `chat.create_res` | `{ "code": 0, "msg": "ok", "data": { "userId": 1, "account": "登录账号", "username": "昵称" } }` | 新建对话结果；成功返回目标用户 `userId`、**`account`** 与昵称；`404` 表示账号不存在 |
| `msg.offline_ack_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 离线消息确认结果 |
| `chat.history_res` | `{ "code": 0, "msg": "ok", "data": { "msgs": [ { "from": "<account>", "fromNick": "...", "content": "...", "ts": 1720000000, "clientMsgId": "...", "messageId": "...", "chatType": "single", "mediaId": "<fileId或空串>" }, ... ] } }` | 单聊历史；`msgs[]` 元素结构与 `msg.recv` 相同（`from` 为发送方登录账号；**媒体消息 `mediaId` 为 fileId 字符串（v3.0）**），**按 ts 倒序（最新在前）**；已加群隔离，群消息不在此返回（群历史用 `group.history_res`） |
| `msg.read_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 已读回执结果 |
| `user.search_res` | `{ "code": 0, "msg": "ok", "data": { "users": [ { "account": "...", "username": "昵称", "isContact": true }, ... ] } }` | 搜索结果；`isContact` 表示目标是否已是我的联系人 |
| `contact.request_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 好友请求结果；`403` 对方未开启添加、`409` 对方已是联系人 / 已存在待处理请求、`400` 不能添加自己、`404` 目标用户不存在 |
| `contact.accept_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 同意好友请求结果；`404` 不存在该待处理请求 |
| `contact.reject_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 拒绝好友请求结果；`404` 不存在该待处理请求 |
| `contact.list_res` | `{ "code": 0, "msg": "ok", "data": { "contacts": [ { "account": "...", "username": "昵称", "online": true }, ... ] } }` | 我的联系人列表；`online` 为该联系人当前是否在线（v2.1，Presence 内存/Redis 双后端） |
| `contact.requests_res` | `{ "code": 0, "msg": "ok", "data": { "requests": [ { "account": "...", "username": "昵称", "ts": 1720000000 }, ... ] } }` | 收到的待处理好友请求列表（按 ts 排序） |
| `contact.delete_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 删除联系人结果；`400` 参数错误 / 不能删自己、`404` 对方不是你的联系人 |
| `settings.get_res` | `{ "code": 0, "msg": "ok", "data": { "allowTempChat": true, "searchable": true, "allowAdd": true } }` | 我的隐私设置三开关（bool） |
| `settings.set_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 更新隐私设置结果 |
| `contact.request` | `{ "from": "<发起方账号>", "fromNick": "发起方昵称", "ts": 1720000000 }` | 收到好友请求的推送（在线直接推送；离线登录后经 `pushPendingRequests` 补发） |
| `contact.added` | `{ "account": "<接受方账号>", "username": "接受方昵称" }` | 好友请求被同意，双方已成为联系人（推送给出请求方） |
| `contact.deleted` | `{ "account": "<删除方账号>" }` | 被对方从联系人删除（推送给对方） |
| `group.create_res` | `{ "code": 0, "msg": "ok", "data": { "groupId": 1, "name": "..." } }` | 建群结果；`400` 参数错误（`name` 为空或超长） |
| `group.invite_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 群主拉人结果；`400` 参数错误 / `404` 群不存在 / 目标用户不存在 / `403` 仅群主可拉人 / 群已满 / `409` 对方不是你的联系人 / 对方已在群中 |
| `group.leave_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 退群结果；`404` 群不存在 / `403` 群主不能退群 / 非群成员 |
| `group.kick_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 踢人结果；`400` 参数错误 / `404` 群不存在 / 目标用户不存在 / 群已解散 / `403` 仅群主可操作 / `409` 不能踢群主 / 目标非群成员 |
| `group.transfer_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 转让群主结果；`400` 参数错误 / `404` 群不存在 / 目标用户不存在 / 群已解散 / `403` 仅群主可操作 / `409` 不能转让给自己 / 目标非群成员 |
| `group.dissolve_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 解散群结果；`400` 参数错误 / `404` 群不存在 / 群已解散 / `403` 仅群主可操作 |
| `group.rename_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 改群名结果；`400` 参数错误（`groupId` 缺失/非整数，`name` 为空或超 32） / `404` 群不存在 / 群已解散 / `403` 仅群主可操作 |
| `group.announce_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 群公告结果；`400` 参数错误 / `404` 群不存在 / 群已解散 / `403` 仅群主可操作 / `413` 消息内容超长（`content` 超 1024 字节，空=清空） |
| `group.list_res` | `{ "code": 0, "msg": "ok", "data": { "groups": [ { "groupId": 1, "name": "...", "memberCount": 2, "isOwner": true, "announcement": "...", "announceTs": 1720000000 }, ... ] } }` | 我的群列表；`isOwner` 表示我是否为群主；`announcement`/`announceTs` 为最新群公告正文与发布时间（v2.0，无公告时为空串/0）；已解散群不列出 |
| `group.members_res` | `{ "code": 0, "msg": "ok", "data": { "members": [ { "account": "...", "username": "昵称", "joinedTs": 1720000000, "isOwner": true, "online": true }, ... ] } }` | 群成员列表；非群成员 `403 非群成员`；`online` 为成员当前是否在线（v2.1） |
| `group.history_res` | `{ "code": 0, "msg": "ok", "data": { "msgs": [ { "from": "<account>", "fromNick": "...", "groupId": 1, "groupName": "...", "content": "...", "ts": 1720000000, "clientMsgId": "...", "messageId": "...", "chatType": "group", "mediaId": "<fileId或空串>" }, ... ] } }` | 群历史（仅加群后、退群前可见，按 ts 倒序）；非（曾）成员 `403 非群成员`；**媒体消息 `mediaId` 为 fileId 字符串（v3.0）** |
| `group.added` | `{ "groupId": 1, "name": "...", "inviter": "<邀请人账号>" }` | 被拉入群的推送（在线直接推送；离线登录后用 `group.list` + `msg.offline` 自洽） |
| `group.member_changed` | `{ "groupId": 1, "change": "join"/"leave"/"kick", "account": "<变动成员账号>" }` | 群成员变动推送（`join` 有人入群 / `leave` 有人退群 / `kick` 有人被踢，v2.0），推送给群内在线成员 |
| `group.removed` | `{ "groupId": 1, "name": "..." }` | 被踢推送（v2.0，仅推送给被踢的在线者） |
| `group.owner_changed` | `{ "groupId": 1, "account": "<新群主账号>" }` | 转让群主推送（v2.0），推送给群内在线成员 |
| `group.dissolved` | `{ "groupId": 1 }` | 解散群推送（v2.0），推送给解散前在线成员 |
| `group.renamed` | `{ "groupId": 1, "name": "..." }` | 改群名推送（v2.0），推送给群内在线成员 |
| `group.announced` | `{ "groupId": 1 }` | 群公告推送（v2.0，正文走 `group.list` 拉取，避免大包扇出），推送给群内在线成员 |
| `media.presign_res` | `{ "code": 0, "msg": "ok", "data": { "fileId": "<32位hex>", "uploadUrl": "http://...&X-Amz-Signature=...", "expiresIn": 900 } }` | 申请上传结果（v3.0）：`fileId` 为媒体唯一标识（32 位 hex），`uploadUrl` 为 PUT 预签名直传 URL，`expiresIn` 为有效期秒（900）；`400` 参数错误（kind/mime/size/fileName 非法）、`413` 文件过大、`429` 请求过于频繁、`503` 对象存储不可用 |
| `media.commit_res` | `{ "code": 0, "msg": "ok", "data": {} }` | 提交结果（v3.0）：成功 `code=0`；`400` 参数错误 / 媒体状态不可提交 / 对象不存在 / 文件大小不匹配、`403` 无权操作该媒体、`404` 媒体不存在、`413` 文件过大、`503` 对象存储不可用；已 COMMITTED 重复提交幂等回 `code=0` |
| `media.url_res` | `{ "code": 0, "msg": "ok", "data": { "urls": { "<fileId>": { "url": "http://...&X-Amz-Signature=...", "expiresIn": 3600 }, ... } } }` | 取 URL 结果（v3.0）：`urls` 为 `fileId → {url, expiresIn}` 映射；单值项成功只含 `{url,expiresIn}`，失败项为 `{code,msg}`（如 `403 无权访问该媒体`/`404 媒体不存在`）；整包 `code` 为 `0`，逐项错误在 `urls` 值内。顶层 `400` 参数错误（缺 `fileId`/`fileIds`、非串/非数组、批量 >100） |
| `error` | `{ "code": 400, "msg": "..." }` | 通用错误事件（简式结构） |

字段细则：

- `msg.recv.chatType` 为 `"single"`（单聊）或 `"group"`（群聊，v1.9）；`msg.offline.msgs[]` 元素结构与 `msg.recv` 相同，同样携带 `chatType`、`clientMsgId`、`fromNick`、`messageId` 与 `mediaId`（v3.0，媒体消息为 fileId 字符串，非媒体为空串）
- **群载荷（v1.9）**：群消息的 `msg.recv` / `msg.offline.msgs[]` / `group.history_res.msgs[]` 元素含 `groupId`（数字群标识）、`groupName`，`chatType` 为 `"group"`，`from` 为发送方 `account`；客户端按 `chatType` 区分单聊与群聊会话
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
| 403 | WRONG_PASSWORD | 账号或密码错误 / **未开启临时对话** / **对方未开启添加** / **拉黑场景（对方已拒收 / 你已拉黑对方，请先解除）** / **群场景（非群成员 / 仅群主可拉人 / 仅群主可操作 / 群主不能退群 / 群已满）** |
| 404 | USER_NOT_FOUND | 目标用户不存在 / **不存在该待处理请求** / **对方不是你的联系人** / **群不存在 / 群已解散** |
| 409 | USER_EXISTS | 登录账号已存在 / **对方已是联系人** / **已存在待处理的请求** / **对方不是你的联系人（拉群）** / **对方已在群中 / 不能踢群主 / 目标非群成员 / 不能转让给自己** / **已拉黑对方 / 未拉黑对方（解除）** |
| 413 | MSG_TOO_LONG | 消息超长（>64KB） / **群公告超长（>1024 字节，文案"消息内容超长"）** |
| 429 | TOO_MANY_ATTEMPTS | 登录失败次数过多被临时限流 |
| 429 | TOO_MANY_MESSAGES | `msg.send` 发送过频被限流（`msg.ack` 携带） |
| 500 | INTERNAL | 服务器内部错误（**预留**，当前版本服务端未返回过该码） |

> `msg` 字段为**中文错误原因**，`code` 不变，客户端应依据 `code` 判断错误类型，`msg` 仅用于展示。各 `code` 对应的 `msg`：
> `400`→无效的封包 / 封包格式错误 / 封包字段错误 / 参数错误 / 未知的指令类型 / 账号须为10-11位数字 / 不能添加自己为联系人；
> `401`→未登录 / 登录凭证无效或已过期；
> `403`→账号或密码错误 / 对方未开启临时对话，请先添加联系人 / 对方未开启添加，请使用其他方式联系 / 非群成员 / 仅群主可拉人 / 仅群主可操作 / 群主不能退群 / 群已满；`404`→目标用户不存在 / 不存在该待处理请求 / 对方不是你的联系人 / 群不存在 / 群已解散；
> `409`→账号已存在 / 对方已是你的联系人 / 已存在待处理的请求 / 对方不是你的联系人 / 对方已在群中 / 不能踢群主 / 目标非群成员 / 不能转让给自己；`413`→消息内容超长（超过64KB） / 消息内容超长（群公告超 1024 字节）；
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
7. **服务端停机**（v2.3）：部署重启或 SIGTERM 时，服务端先向所有在线连接发送 WS Close 帧（状态码 **1001 Going Away**）再关闭连接；客户端收到 1001 应按"服务端维护"提示用户并进入重连退避（期间消息已落库，重连后离线补发/历史拉取自愈）。停机前服务端会把已入队的消息任务处理完（排水，超时 5 秒放弃），回复不出的回执不丢数据

## 9. 三端一致约定

- Qt（QWebSocket）与 Web（浏览器 WebSocket）客户端必须严格使用本协议的 `type`、`data` 字段与错误码，保证互通
- **用户模型约定**：登录、收发消息、历史、已读全部用 `account`；`userId` 对客户端不透明（服务器内部用）；界面展示用 `username`（昵称，来自响应或 `fromNick`）
- `seq` 建议客户端按会话自增；服务端不依赖其顺序，仅用于客户端本地对齐
- `clientMsgId` 由发送端生成（如 UUID），用于去重与 ACK 对齐；同一发送方窗口内重发相同 id 只投递一次
- 客户端应忽略未知字段，以兼容协议后续扩展
- **持久化现状**：用户（users 表）、消息/离线/历史/已读（messages 表）、媒体元数据（media 表，v3.0）已持久化到 MySQL（重启不丢失）；token/发送限流/幂等去重均可存 Redis（`IM_REDIS_ENABLED=1` 启用，跨重启保留；未启用则内存）；在线状态（Presence）当前仍为内存

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
| `IM_MYSQL_USER` / `IM_MYSQL_PASSWORD` | `IMS_jack` / 空（须显式注入） | MySQL 账号与密码；密码**不再内置**，经环境变量/`.env`（compose 自动读取，git 忽略）注入 |
| `IM_MYSQL_DB` | `IMS_reactor_db` | 数据库名 |
| `IM_REDIS_ENABLED` | `0` | `1` 启用 Redis 存储（token/限流/幂等） |
| `IM_REDIS_HOST` | `127.0.0.1` | Redis 主机（Docker 内填 `redis`） |
| `IM_REDIS_PORT` | `6380` | Redis 端口（Docker 容器内部为 6379） |
| `IM_REDIS_PASSWORD` / `IM_REDIS_DB` | 空 / `0` | Redis 认证与库号 |
| `IM_MAX_PER_IP_CONN` | `100` | 单 IP 并发连接上限（`0` 表示不限） |
| `IM_OFFLINE_PURGE_INTERVAL` | `1800` | 离线消息 TTL 清理间隔（秒）；离线消息保留 7 天，超期由后台任务删除 |
| `IM_LOG_FILE` | 空（stderr） | 日志输出文件（追加） |
| `IM_LOG_LEVEL` | `INFO` | 日志级别：`DEBUG` / `INFO` / `WARN` / `ERROR` |
| `IM_LOG_ROTATE_BYTES` | `52428800` | 日志按大小轮转阈值（字节，50MB；`0` 不轮转） |
| `IM_LOG_KEEP` | `5` | 轮转保留份数（`app.log.1` … `.5`） |
| `IM_TRUSTED_PROXIES` | 空 | 可信反向代理 IP 列表（逗号分隔）；对端在列表内时握手日志显示 `X-Forwarded-For` 真实客户端 IP，未配置则忽略该头（防伪造）。见 `docs/nginx-deploy.md` |
| `IM_S3_ENDPOINT` | 空 | 对象存储端点（host[:port]，不含 scheme，如 `minio:9000`）；**为空则媒体功能降级**（`media.*` 回 503 对象存储不可用，消息收发不受影响） |
| `IM_S3_SCHEME` | `http` | 对象存储访问协议（`http`/`https`；生产用 `https`） |
| `IM_S3_REGION` | `us-east-1` | 对象存储区域（SigV4 签名用） |
| `IM_S3_BUCKET` | `media` | 对象存储桶名 |
| `IM_S3_ACCESS_KEY` / `IM_S3_SECRET_KEY` | 空（须显式注入） | 对象存储 AK/SK，经环境变量/`.env` 注入；生产必须配强随机值 |
| `IM_S3_PATH_STYLE` | `1` | path-style 访问（MinIO 需 `1`；`0` 为 virtual-hosted style） |
| `IM_MEDIA_GC_INTERVAL_SEC` | `86400` | 媒体 PENDING 记录 GC 两次执行的最短间隔（秒；测试用小值验证） |

MySQL 连接失败启动报错退出；`IM_REDIS_ENABLED=1` 且连接失败同样报错退出。

### 10.3 Docker 部署
仓库含 `Dockerfile` 与 `docker-compose.yml`：`mysql:8.4` + `redis:8-alpine` +
`minio`（对象存储）+ `imserver` + `nginx` 五服务编排（部署标准，按远程生产机固化）。
```
docker compose up -d --build
```
- **部署标准**：连接上限 50（2C2G）、imserver 仅绑 `127.0.0.1:9191`
  （对外唯一入口 Nginx 8080）、`IM_TRUSTED_PROXIES=网桥 IP`（换机器核对
  网段）、MySQL buffer pool 128M、密码经 `.env` 注入（compose 自动读取
  同目录 `.env`）、`IM_MAX_PER_IP_CONN=0`（经 Nginx 对端恒为网桥 IP，
  入口限流交给 Nginx perip 50）
- imserver 等 MySQL 健康后再启动（`depends_on + service_healthy`），并带
  `restart: unless-stopped`（启动连不上 DB 自动重试拉起）
- 容器内 imserver 通过服务名连接：`IM_MYSQL_HOST=mysql`、`IM_REDIS_HOST=redis`（端口 6379）
- `mysql_data` / `redis_data` 命名卷持久化数据
- `docker compose` 启用 Redis（`IM_REDIS_ENABLED=1`），token 免密重连跨容器重启不失效
- **minio**（`minio/minio`）：开发/CI 标准对象存储，供 `IM_S3_ENDPOINT=minio:9000` 内网直连；AK/SK 经 `.env`（`IM_S3_ACCESS_KEY`/`IM_S3_SECRET_KEY`，缺省 `minioadmin` **仅本地/CI 可用**）；`minio_data` 命名卷持久化；`createbuckets` 一次性服务用 `minio/mc` 建 `media` 桶（S3 不因 PUT 自动建桶）。**CI 的 MinIO 实际用静态二进制**（本仓库 Release 资产 `vendor-binaries`，API 端点 + `GITHUB_TOKEN` 认证下载 + sha256 校验）：`minio/minio` 镜像匿名拉取被拒、`dl.min.io` 浮动路径 410 下线、`dl.minio.org.cn` 对 GitHub runner 403（均 2026-10-02 实测）——compose 的 minio 服务保留作标准环境定义；生产可换阿里云 OSS 等 S3 兼容服务（V4 适配，签名接口通用）
- **nginx**（`nginx/im.conf`，8080 反代 9191）：WebSocket 升级头、90s 读超时、
  每真实 IP 50 连接限流、`X-Forwarded-For` 传递；**v3.0 已下线 `/uploads/` 静态托管**（图片改对象存储直传，不再经服务端本地盘服务）；TLS/域名见 `docs/nginx-deploy.md`

### 10.4 云服务器部署（以阿里云 2 核 2G 为例）

同机部署 MySQL + Redis + imserver 全家桶（推荐 Docker 方式）时，2C2G **够用但需调优**：

```bash
# 启动参数（第 4 个参数设全局连接上限，2G 内存机器不限连接有 OOM 风险）
./imserver 9191 2 3 500    # 端口 9191 / IO 线程 2 / 工作线程 3 / 最大连接 500
```

| 调优项 | 建议 | 原因 |
|---|---|---|
| 全局连接上限 | `500`（必设，默认 0 不限） | 每连接最坏配额 16MB（读队列 8MB + 写缓冲 8MB），不限连接会被刷爆内存 |
| 线程数 | IO 2 / 工作 3 | 2 核机器减少线程栈开销与调度竞争 |
| MySQL | `innodb_buffer_pool_size=128M`；开 1G swap 兜底 | 2G 内存中 MySQL 约 500MB，防涨占 |
| 带宽 | 按流量计费或 ≥3Mbps | 1Mbps 固定带宽 ≈128KB/s，历史拉取（一次最多 200 条）与大消息并发会排队数十秒 |
| 实例类型 | 突发性能实例（CPU 积分制）可接受 | IM 空闲为主；压测/群发高峰被限频属预期 |

**容量参考（调优后）**：同时在线 2000-5000 连接（epoll 事件驱动，空闲连接近零成本）；
消息吞吐约 500-1000 条/秒（瓶颈为仓库层单 MySQL 连接串行化，非机器规格）。
适用个人项目/演示/百人级使用；万人级需先解决单连接瓶颈与水平扩展（当前非目标）。

**内存分布参考**：系统 ~250MB + MySQL ~500MB + Redis ~50MB + imserver 200-400MB，余量约 800MB。

## 11. 安全说明（2026-09-08 审查结论）

### 11.1 已确认安全
- **无 SQL 注入**：全仓 SQL 经 `mysqlStmtExec` 参数化（`?` 绑定），用户字符串无一拼接进 SQL
- **密码存储**：PBKDF2-HMAC-SHA256（10 万次迭代）+ 每用户独立 128 位盐，非明文
- **Token**：256 位随机生成，7 天过期，登出即失效，内存/Redis 只存 SHA-256 摘要
- **无越权**：`msg.send/history/read/offline_ack`、`group.*`、联系人、设置接口均按 `session->userId()` 校验身份与成员关系，遍历 `userId`/`groupId` 不可读他人数据；**媒体接口（v3.0）**：`media.commit` 仅上传者可提交、`media.url` 仅消息相关方可取（越权 403）
- **登录不泄露存在性**：账号不存在与密码错误统一回 `403 账号或密码错误`
- **传输层防护**：单消息 2MB 上限、握手 10 秒超时、僵尸连接 60 秒踢除、写缓冲 8MB 防慢消费

### 11.2 已知限制（2026-09-09 加固后）
以下高/中危项已修复；低危项为已知取舍，暂保持：
- **已修复**：`msg.offline_ack` 的 `messageIds` 单次上限 1000（超限回 `400 确认数量超限`）；登录限流表淘汰跳过被锁定账号（洗表不再清零受害者）；`clientMsgId` 上限 64 字节（与 DB 列宽对齐，超限回 `400 参数错误`，单聊/群聊同规则）；单聊离线补发上限 100 条/次（激活 `kOfflineMaxPerUser`，超量分多次登录渐进补发）；单 IP 并发连接上限 100（`IM_MAX_PER_IP_CONN` 可覆盖，`0` 不限）
- **低（保持）**：JSON 无嵌套深度限制；群发扇出约 49 倍出流量放大（建群需 49 个联系人，门槛较高）；登录后 `404` 可探活账号（IM 常见设计）；~~compose 与缺省配置含明文密码~~ **已脱敏**（2026-09-29：模板改 `${IM_MYSQL_PASSWORD:-change-me}` 占位、源码缺省置空、测试读环境变量；git 历史中旧开发密码 520010 仍可考古但无生产价值，生产仍须强密码注入）；Redis 缺省无密码（靠 `127.0.0.1` 绑定与内网隔离）；MySQL 单账号全权（未做最小授权）

### 11.3 没做的功能（直白版，2026-09-16 审计）
- 群没有管理员：只有群主能管人，没有"管理员"这个角色；也没有禁言、入群审批、群文件
- 公告只有一条：新公告直接盖掉旧的，没有公告列表可翻
- ~~`heartbeat` 指令发了会报错~~ 已从协议文档移除：保活用的是 WebSocket 自带的 Ping/Pong，够用了，业务层就不做了
- 被踢/解散这类通知，离线收不到：比如你断网时被踢出群，重登不会补这条通知，
  自己拉一下群列表就知道了，消息历史不受影响
- 多端"同步"只覆盖在线端（漫游已定为非目标）：你用电脑发消息时，如果手机那会儿离线，
  手机重新登录后**不会自动收到你电脑上发的那条**——消息没丢（在历史里），
  客户端重登后拉一次聊天记录就能看到。这与微信登录时**不勾选"同步聊天记录"**
  的行为一致：不做服务端自动漫游，由客户端按需拉取（2026-09-28 决策）
- 登录限流计数在内存（进程重启清零）：防的是在线爆破；重启洗白需要攻击者
  具备重启进程的能力（彼时已沦陷），跨重启持久化不做（2026-09-30 自检明确）
- 大群拉成员列表有点慢（已优化）：群信息提出循环只查一次，在线状态改批量查询（Redis 一次 MGET / 内存单锁），50 人群拉成员不再是 50 次往返
- 拉黑的三个取舍（v2.4）：`user.search` 拉黑后仍可搜到对方（搜索是目录≠通信）；`msg.send` 的「对方已拒收」对被拉黑方直接暴露被拉黑事实（产品选择，与好友请求的模糊策略不一致）；`msg.read_notify` 是读者自己多端的未读同步、从不推对端，故拉黑不涉及它（`chat.history` 的 `readTs` 属"历史不动"语义）

### 11.4 待修的缺陷（直白版，2026-09-16 审计；2026-09-27 更新）
1. ~~给不登录的人发消息，数据库会越涨越大（中）~~ **已修复**：新增离线 TTL 清理线程，
   超 7 天未领取的离线消息自动删除（间隔 `IM_OFFLINE_PURGE_INTERVAL`，默认 30 分钟）；
   曾有单位 bug（168 小时被当 168 秒，离线 3 分钟即删），`22077b1` 修复
2. ~~v2.0 升级脚本只能跑一次~~ **已修复**：迁移脚本改为逐列守卫（存在即跳过），可重复执行
3. 直接改数据库写超长公告会被悄悄截断（低）：正常发公告（≤1024 字节）没事，
   只有绕过服务器手工写库且超过 2048 字节才会截（接受：正道上不可达）
4. ~~测试脚本清理失败看不见报错~~ **已修复**：清理失败输出警告到 stderr
5. ~~个别测试只验了结果码没验类型~~ **已修复**：7 处弱断言全部补 `type` 校验
6. ~~Redis 无超时 + 锁内 IO 级联~~ **已修复**：Redis 读写/连接超时 1 秒（假 Redis 实测登录 1.2s、
   列表 1.0s 降级返回不挂死）；在线标记调用移出互斥锁；失败统一按离线降级
7. ~~列表接口 N+1 查询~~ **已修复**：`contact.list`/`group.members` 在线状态改批量查询
   （Redis 一次 MGET / 内存单锁）；`group.members` 群信息提出循环只查一次
8. ~~read_notify 账号枚举（低）~~ **已修复**：`msg.read_notify` 仅在双方确有单聊往来时
  推送（查询失败按无往来 fail-closed），数字 userId 枚举账号的回显通道关闭

### 11.5 架构遗留收敛（直白版，2026-09-29）
本轮把 2026-09-29 架构审视揪出的四项"不疼但一直在"的问题全部处理掉：
1. ~~指令路由 29 层 if-else~~ **已改哈希表**：`routes_` 一次性注册、O(1) 查表，
   加新指令不再堆 `appserver.cpp` 的路由函数
2. ~~联系人/群成员缓存无失效比对（绕过服务端直改库不自知）~~ **已加低频比对**：
   清理线程每轮（默认 30 分钟）对库 COUNT 与缓存条目数，不一致全量重建；
   重建先查全量、成功才换入（查询失败保持旧缓存，不会把缓存打成空）。
   **边界**：比对信号是行数——直改库若同时删一行又加一行（计数恰好
   平衡）则检不出（正常运维不会这么改库；绕库直改本就是越界操作）
3. ~~连接按 `fd % N` 分配 IO 线程（负载可能倾斜）~~ **已改最少连接优先**：
   accept 时选当前连接数最少的从 loop；顺带接受关闭时扣减计数（幂等）
4. ~~停机直接断 TCP（队列任务丢弃、无通知）~~ **已改四步优雅停机**：
   停接入 → 发 Close(1001)（在线客户端收到"维护中"信号）→ 工作队列排水
   （带 5 秒超时防挂死）→ 兜底关闭；线程池 stop 从"置位即丢队列"升级为
   "清空队列才退"（drain 语义）+ 超时放弃
   注：工作线程池的 drain 语义在旧实现里已存在（worker 取空队列才退出），
   本轮补的是超时保护与停机顺序修正（先关接入再排水，避免排水期间新任务入队）

至此架构遗留仅剩规模类（单 MySQL 连接、群发水位线串行写、仓库层独占锁——
见改进计划 C 区，用户量证明需要时再做）与已定取舍类（单实例、漫游非目标等）。

### 11.6 生产故障修复：MySQL 断线自愈（直白版，2026-09-29）
**事故**：部署机闲置约 19 小时（> MySQL wait_timeout 8 小时），服务端把连接
踢掉（errno 4031）。imserver 是单条长连接、无自愈——第一次查询失败后
**之后所有登录持续失败**，只能重启进程恢复。
**修复**：`MySqlClient::stmtExec/query` 两个执行入口在失败且 errno 属于
"连接已断"类（2006/2013/4031）时，在原 handle 上自动重建连接并重试一次
（连接参数备份于成员；重建后 handle 指针不变，调用方无感知）。四个仓库
（用户/消息/联系人/群）共 4 条连接全部经此入口，统一自愈。
**验证**：biz 新增 4 用例——`KILL` 服务端 MySQL 连接（模拟被踢）后登录/
发消息/联系人列表全部透明恢复；重连只重试一次防重连风暴。
**自检补充（2026-09-30）**：新增整库宕机→恢复闭环用例（docker stop/start
真实模拟 MySQL 整体不可用）：宕机期间请求优雅失败（500），恢复后服务
**自动跟上无需重启**。自检修复一处自愈引入的退化：重连失败曾置连接状态
为不可用，叠加各仓库的状态短路守卫会导致"MySQL 恢复后永久瘫痪"——
已改为重连失败不关门（连接状态仅表达构造时结果）。
**边界**：查询中途被杀的语句（半途失败的那条）按失败返回——离线补发
（非破坏性）与客户端重试兜底；自愈只救"连接"，不重放"已失败的语句语义"
（INSERT 类由 clientMsgId 幂等挡住重复）。

**停机看门狗**（2026-09-30 自检防御）：优雅停机各环节均有超时（排水 5s、
MySQL IO 5s、重连 1 次），正常 <15 秒必然完成；main 起看门狗线程，
15 秒后硬退出兜底——保证任何未预见挂点下进程可退出（compose
restart 依赖进程退出才能拉起）。
**运维提示**：MySQL 整体停机期间，清理线程每轮（默认 30 分钟，测试 2 秒）
会对四条连接各产生一条"重连失败"错误日志——属预期的降级噪音，
非故障信号（恢复后自愈，见下条）。
**测试盲区说明**：整库宕机→恢复用例（docker stop/start 真实模拟）只在
有本机 docker 的环境执行；GitHub Actions 的 mysql 是 service 容器
（无 docker CLI），该用例在 CI 跳过——宕机恢复路径靠本地矩阵覆盖。

**自检三轮加固（2026-09-30，外部审计 A/B/C 分级全量处置）**：
- **A1 注册限流 + PBKDF2 并发闸**：auth.register 按 IP 3 次/10 分钟
  （`IM_REGISTER_LIMIT_PER_WINDOW` 可覆盖；未登录即可调用的 PBKDF2
  50-100ms/次此前可被刷成未认证 CPU DoS）；登录/注册的慢哈希同时受
  全局并发闸（同时最多 2 个在算，排队不占 CPU）
- **B2 登录拆锁**：PBKDF2 移出 UserStore 互斥锁（此前 5 个并发登录可让
  全部消息投递停顿半秒——热路径 lookupAccount 共用该锁）
- **B3 用户缓存自愈**：ids/idAccounts 缓存纳入 syncCaches 计数比对
  （此前是唯一无自愈路径的缓存：加载失败即 fromNick/数字 userId 404
  到重启）；MySqlUserRepository 补内部锁（清理线程直调路径安全），
  并修复插锁时引入的 fetchUserInfo→fetchUser 同线程递归加锁自死锁
  （gdb 现场抓栈定位）
- **B4 群满 TOCTOU**：activeCount 与 addMember 合并为锁内原子
  addMemberCapped（并发拉人不再能击穿 50 上限到 51）
- **B1 固定容器 IP**：compose 自定义网段 172.19.0.0/24，nginx 容器
  固定 .5——IM_TRUSTED_PROXIES=网关+nginx 两值，8080/80 两路径的
  X-Forwarded-For 信任均稳定成立（容器重建不再漂移失效）
- **C 批**：stop_grace_period 20s（＞看门狗 15s，排水不被 SIGKILL 截
  断）、workerLoop/purgeLoop catch(...) 兜底（异常不再 terminate 全
  进程）、user.search 纳入查询限流（LIKE 可刷）、RedisRateLimiter::reset
  用构造前缀（qrate 不再删错桶）、Presence DECR 失败不误删在线键、
  argv[4] 非数字警告、MySQL 连接禁用 TLS（libmysqlclient TLS 握手库
  级泄漏，LSan 实测 37982 次分配不回收——内网容器网络可接受，公网
  入口由 Nginx wss 终结；非 TLS 下密码走 RSA 公钥交换仍不裸奔）

**消息唯一性防线**（2026-09-30 自检修复）：`messages.message_id` 升级为
**数据库唯一索引**（其余四表本就有唯一键，唯 messages 漏配）。防的是
自愈 INSERT 重试的"半途成功"场景：连接断时语句可能已在服务端执行，
重试会插入重复消息（进离线补发/历史）——加唯一键后撞键被拒
（errno 1062 非连接类错误，不重试），客户端收失败回执后重试/离线
补发兜底（宁拒不重）。存量库迁移见
`db/migrate_unique_message_id.sql`（幂等：先查重复再改索引）。
`nextMessageId` 起点随机化：id 为 `<秒级时间戳>-<seq>`，seq 固定从 1
起会在同秒重启前后生成相同 id（撞键丢消息），随机起点把窗口概率
降到约百万分之一。

**运行期自愈能力总表（2026-09-29 收敛后）**——正常停机/闪断类故障
**均无需重启进程**：

| 故障 | 行为 | 需要重启 |
|---|---|---|
| MySQL 空闲被踢（4031）/闪断（2006/2013） | 下次查询透明重建重试 | 否 |
| MySQL 短暂停机 | 停机窗口内请求回 500（进程不挂死）；恢复后自动跟上 | 否 |
| Redis 宕机/闪断 | 1 秒超时降级（消息收发不受影响）；恢复后每次命令前检查重建 | 否 |
| 单客户端异常/慢消费 | 只断该连接（写缓冲背压） | 否 |
| MySQL 停机时停机 | purge 线程 5s 超时 + 排水超时 | 否 |
| **启动时** DB 不可用 | fail-fast 退出（设计行为）；compose `restart: unless-stopped` 自动拉起重试直到连上 | 自动（免人工） |
