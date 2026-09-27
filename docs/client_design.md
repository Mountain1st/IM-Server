# IM 客户端设计文档（Windows / 协议 v2.2）

> 实际客户端开发在 Windows 环境进行，本文只定义设计、模块与接口，不含实现。
> 协议对接以 `docs/protocol.md`（v2.1）与 `docs/client_api.md` 为唯一事实来源，客户端已同步至群聊 v2.0（含群管理）与 v2.1（在线状态字段）。

## 1. 技术选型

| 项 | 选择 | 理由 |
|---|---|---|
| 语言/框架 | C++17 + Qt 6（Widgets） | 与服务端同语言，信号槽天然适配推送模型；QWebSocket 免手写协议层 |
| 构建 | CMake + MSVC 或 MinGW | 与服务端构建体系一致 |
| JSON | QJsonDocument | Qt 自带 |
| 存储 | SQLite（会话缓存） + QSettings（凭据） | 历史缓存与免密重连 |
| 依赖 | OpenSSL（Qt 网络模块需要） | `wss://` 预留 |

## 2. 模块划分

```text
┌────────────────────────────────────────────┐
│ UI 层：登录窗 / 会话列表 / 聊天窗 / 群管理窗   │
├────────────────────────────────────────────┤
│ ViewModel：会话模型（QAbstractListModel）    │
│  单聊会话键=对方 account；群会话键=groupId    │
├────────────────────────────────────────────┤
│ 业务层：AuthSvc / ContactSvc / ChatSvc      │
│  / GroupSvc / SettingsSvc（指令封装）        │
├────────────────────────────────────────────┤
│ 连接层：WsClient（QWebSocket 封装）          │
│  自动重连 + 免密 token + seq 配对            │
├────────────────────────────────────────────┤
│ 存储层：MsgCache（SQLite）/ TokenBox        │
└────────────────────────────────────────────┘
```

## 3. 连接层接口（核心）

```cpp
class WsClient : public QObject {
  Q_OBJECT
public:
  void connectToServer(const QUrl& url);       // wss/ws
  void send(const QString& type, const QJsonObject& data);  // 自动配 seq
signals:
  void connected();                            // 握手完成
  void disconnected(int closeCode);            // 触发自动重连（指数退避，上限 30s）
  void responseReceived(const QString& type,
                        const QJsonObject& data);  // *_res（按 seq 配对）
  void eventReceived(const QString& type,
                     const QJsonObject& data);      // 推送（msg.recv/group.* 等）
private:
  void reconnect();                            // 重连成功后自动 auth.token 免密登录
  int nextSeq();                               // 请求序号；res 与 req 按 seq 配对
};
```

**重连语义**（与服务端配套）：
1. 断线 → 指数退避重连（1s/2s/4s…上限 30s）
2. 连上后先 `auth.token`（token 从 TokenBox 取，7 天有效）
3. token 失效（401）→ 通知 UI 弹登录窗
4. 登录成功后服务端自动补发离线消息（`msg.offline`），客户端逐条入会话模型后回 `msg.offline_ack`（单次 ≤1000 条）

## 4. 业务层接口

```cpp
class AuthSvc {      // 注册/登录/登出
public:
  void login(const QString& account, const QString& password);
  void logout();
signals: void loginResult(int code);  // code=0 成功
};

class ContactSvc {   // 联系人（v1.8/v1.8.1）
public:
  void search(const QString& keyword);
  void requestContact(const QString& account);   // 加好友
  void acceptContact(const QString& account);
  void listContacts();                           // 含 online 字段（v2.1）
signals: void contactsUpdated(const QJsonArray& contacts);
  void requestReceived(const QString& account);  // contact.request 推送
};

class ChatSvc {      // 单聊（v1.7 account 定位）
public:
  void send(const QString& toAccount, const QString& text);
  // 自动生成 clientMsgId（QUuid，≤64 字节）；超时未 ack 触发重发（幂等保证不重复）
  void history(const QString& toAccount, int limit = 50);
  void markRead(const QString& fromAccount, quint64 ts);
};

class GroupSvc {    // 群聊 v1.9 + 群管理 v2.0
public:
  void create(const QString& name);
  void invite(qint64 groupId, const QString& account);
  void leave(qint64 groupId);
  void kick(qint64 groupId, const QString& account);
  void transfer(qint64 groupId, const QString& account);
  void dissolve(qint64 groupId);
  void rename(qint64 groupId, const QString& name);
  void announce(qint64 groupId, const QString& content);  // 空=清空
  void list();                                             // 含 announcement/announceTs
  void members(qint64 groupId);                            // 含 online（v2.1）
  void history(qint64 groupId, int limit = 50);
  void sendToGroup(qint64 groupId, const QString& text);   // chatType="group"
};
```

## 5. 事件驱动（服务端推送 → UI）

| 推送 | UI 动作 |
|---|---|
| `msg.recv`（chatType=single/group） | 会话窗新增气泡；群聊按 `groupId` 路由 |
| `msg.offline` | 逐条入库入模型后回 `msg.offline_ack` |
| `group.added` / `group.removed` | 刷新群列表；removed 弹提示 |
| `group.member_changed`（join/leave/kick） | 成员列表刷新 |
| `group.owner_changed` / `renamed` / `announced` / `dissolved` | 群信息窗刷新/公告条更新 |
| `contact.request` / `contact.added` / `contact.deleted` | 好友请求红点 / 列表刷新 |

## 6. 跨端状态同步（v2.2 已实现）

**能实现，服务端代价小**。当前缺口：A 端已读不会通知 B 端；一端发消息其他端不回显自己的消息。方案：

**服务端（两处小改，均为现有模式复用）**
1. **已读同步**：`onRead` 成功后，向该用户**其他在线端**推送 `msg.read_notify {from, readTs}`（复用 `onlineMgr_.sessions(uid)` 广播，排除发起端）。客户端收到后把对应会话未读清零。
2. **自发消息多端回显**：`onSend` 落库后向**发送者其他端**也投递 `msg.recv`（当前仅接收方收到）。多端会话视图一致。
3. 一致性兜底：跨端事件**不要求 ACK**——丢了靠既有机制自愈（重新登录补发 + `chat.history` 全量拉取）。在线状态已有 `contact.list` 的 `online` 字段（v2.1），无需推送。

**客户端**：`msg.read_notify` 与自发 `msg.recv` 走同一事件通道进会话模型，无新增接口。

## 7. 交付边界（本文档不含）

- Windows 打包（windeployqt / 安装包）
- 消息气泡富文本（表情/图片）——协议 content 仍为纯文本
- 桌面通知、托盘、开机自启
