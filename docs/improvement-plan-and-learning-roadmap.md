# IM 项目学习路线与技术学习地图

本文按当前 v3.3 代码整理「学什么、按什么顺序学、在哪个文件验证」，面向希望系统掌握本项目全部技术栈的读者。

> **文档基准：** `HEAD` 为 `0a78003`（2026-10-03），协议以 `docs/protocol.md`（v3.3）为准。架构细节见同目录 `server-architecture-learning-guide.md`。

## 1. 项目现状速览

当前项目是一个单进程、C++17、Linux `epoll` ET 的 WebSocket IM 服务端，技术栈横跨：

```text
C++17 / CMake / CTest
Linux socket / epoll ET / eventfd / timerfd
主从 Reactor / 线程池 / 原子与锁 / 智能指针
HTTP Upgrade / WebSocket RFC 6455 / JSON
MySQL / 参数化 SQL / 断线自愈
Redis / ZSET 滑窗 / SET NX / 连接计数
MinIO/S3 / AWS SigV4 / 预签名直传
Nginx / Docker Compose / CI / Sanitizer
可靠投递语义 / 幂等 / 限流 / 水位线与游标
```

原先改进计划中的工程项大部分已落地：

| 原计划项 | 当前状态 |
|---|---|
| 严格 WebSocket 校验（MASK/RSV/opcode/UTF-8/Close code） | 已实现，三态解析 |
| 握手严格化（方法/Host/Key/版本/大小写） | 已实现 |
| Closing 状态与写完关闭 deadline | 已实现 |
| 资源预算集中（`server_config.hpp`） | 已实现 |
| 认证安全（PBKDF2、token 摘要、登录限流） | 已实现 |
| 幂等与限流顺序修正 | 已实现 |
| 消息持久化与离线可靠补发 | 已实现（MySQL 状态机） |
| 单元测试 / 集成回归 / Sanitizer / CI | 已实现 |
| 结构化日志 / 指标 / `/health` | 已实现 |
| Repository 接口与 MySQL/Redis 适配 | 已实现 |

剩余的演进方向见第 9 节。

## 2. 前置知识对照

按「缺什么补什么」选择入口：

| 缺失基础 | 先补内容 | 对应项目知识 |
|---|---|---|
| 没学过 C++ | 语法、类、RAII、移动语义、STL、Lambda、异常 | 全部源码 |
| C++ 但不熟并发 | mutex、atomic、条件变量、内存模型、智能指针 | Session、ThreadPool、Connection |
| 没学过 Linux 网络 | socket、非阻塞、epoll、信号、fd | Reactor、TcpServer、Connection |
| 没学过 HTTP | 请求行、Header、状态码、Upgrade、反向代理 | WebSocket 握手、nginx |
| 没学过 WebSocket | RFC 6455 帧格式、掩码、分片、控制帧 | websocket.cpp、session.cpp |
| 没学过 SQL | 表/索引/事务、prepared statement | db/schema.sql、六个 MySQL 仓储 |
| 没学过 Redis | 过期、ZSET、原子命令、`SET NX` | 双后端存储 |
| 没学过对象存储 | S3 预签名、SigV4、HEAD | 媒体系统 |
| 没接触过可靠性语义 | 至少一次、幂等、游标、水位线 | 可靠投递 v3.3 |
| 没接触过运维 | Docker Compose、Nginx、CI、Sanitizer | 部署与测试 |

## 3. 技术点与源码映射

### 3.1 网络与并发

| 技术点 | 源码 |
|---|---|
| epoll ET、eventfd、timerfd | `eventloop.*`、`channel.*` |
| 主从 Reactor、IO 线程池 | `tcpserver.*`、`eventloop_thread*.*` |
| 业务线程池与排水停机 | `threadpool.*`、`appserver.cpp` |
| 非阻塞读写、背压、写完关闭 | `connection.*` |
| 双栈监听、连接分配、单 IP 限流 | `tcpserver.*` |
| Session 串行队列与生命周期 | `session.*` |
| 停机四步与看门狗 | `main.cpp`、`tcpserver.cpp`、`appserver.cpp` |

### 3.2 协议

| 技术点 | 源码 |
|---|---|
| HTTP Upgrade 与握手严格校验 | `websocket.*` 握手部分、`session.cpp` |
| 三态帧解析、掩码、RSV、长度编码 | `websocket.*` |
| UTF-8 与 Close code 校验 | `websocket.cpp`、`websocket.hpp` |
| 分片重组、控制帧、心跳、握手超时 | `session.*` |
| JSON envelope 与错误结构 | `message.*` |
| 协议契约与版本史 | `docs/protocol.md`、`docs/client_api.md` |

### 3.3 存储与中间件

| 技术点 | 源码 |
|---|---|
| 参数化 SQL、超时、断线自愈 | `mysql_client.*` |
| Repository 模式与六个 MySQL 仓储 | `*_repository.*`、`mysql_*_repository.*` |
| 消息状态机、投递游标、撤回、可达性 | `message_repository.hpp`、`mysql_message_repository.cpp` |
| Redis 封装与四类键 | `redis_client.*`、`redis_*` |
| Presence 与多端在线 | `presence_store.hpp`、`redis_presence_store.*`、`online_manager.*` |
| 限流与幂等双后端 | `rate_limiter_store.*`、`msg_dedup.*` 及 Redis 版 |
| 缓存自愈 | `appserver.cpp` 的 `syncCaches()` |
| 对象存储与 SigV4 | `object_store.hpp`、`minio_object_store.*`、`sigv4.*`、`media_service.*` |

### 3.4 业务与可靠性

| 技术点 | 源码 |
|---|---|
| 哈希路由与域拆分 | `appserver.*`、`app_*.cpp` |
| 认证与登录限流 | `app_auth.cpp`、`user_store.*` |
| 单聊可靠投递全链路 | `app_message.cpp` |
| 群聊、群管理与连续前缀 ACK | `app_group.cpp`、`group_repository.*` |
| 联系人、隐私、拉黑 | `app_contact.cpp`、`app_block.cpp`、`app_settings.cpp` |
| 撤回与会话列表 | `app_message.cpp`、`mysql_message_repository.cpp` |
| 媒体直传 | `appserver.cpp` 媒体 handler、`media_service.cpp` |
| 确认制与双 ACK | `server_config.hpp`、`app_message.cpp`、`mysql_message_repository.cpp` |

### 3.5 工程与运维

| 技术点 | 源码/脚本 |
|---|---|
| 资源预算 | `server_config.hpp` |
| 日志与轮转 | `log.hpp` |
| 指标与 `/health` | `metrics.*`、`session.cpp` |
| 单元测试（13 个） | `tests/`、`CMakeLists.txt` |
| 业务回归与压测 | `scripts/biz_test.py`、`scripts/bench.py` |
| 一键 CI | `scripts/ci.sh`、`.github/workflows/ci.yml` |
| 部署 | `docker-compose.yml`、`compose.dev.yaml`、`nginx/im.conf`、`Dockerfile` |
| 数据库与迁移 | `db/schema.sql`、`db/migrate_*.sql` |

## 4. 分阶段学习路线

### 阶段 0：跑起来（1-2 天）

目标：先建立「系统真的能转」的体感，再读代码。

步骤：

1. 读根 `README.md` 建立全貌（注意口径偏旧）。
2. 读 `docs/protocol.md`（v3.3）第 1-5 节，理解用户模型与指令表。
3. 起依赖并建库：

   ```bash
   docker compose -f compose.dev.yaml up -d
   MYSQL_PWD='<密码>' mysql -h 127.0.0.1 -u IMS_jack IMS_reactor_db < db/schema.sql
   ```

4. 构建并运行：

   ```bash
   cmake -B build && cmake --build build -j4
   ./build/imserver 9191 2 3 50
   ```

5. 跑一键 CI：`bash scripts/ci.sh`；可选 `--full`（ASan）。
6. 跑压测：`python3 scripts/bench.py`。

验收：能说清一次 `auth.login` 和一次 `msg.send` 分别经过哪些组件与存储。

### 阶段 1：网络核心（3-5 天）

阅读顺序：

1. `src/server_config.hpp` — 先看全部预算与开关。
2. `src/main.cpp` + `src/log.hpp` — 参数、环境变量、信号、停机与看门狗。
3. `src/eventloop.*` + `src/channel.*` — ET、eventfd、timerfd、UAF 防护。
4. `src/eventloop_thread*.*` + `src/threadpool.*` — 两类线程池。
5. `src/tcpserver.*` — 双栈、最少连接分配、三段停机。
6. `src/connection.*` — 背压、`SendResult`、写完关闭。

验收问题：

- 为什么新连接使用最少连接优先，而不是取模？
- ET 模式下为什么必须循环读到 `EAGAIN`？
- 写缓冲超限时 `send()` 返回什么？调用方如何知情？

### 阶段 2：WebSocket 协议（3-5 天，核心）

阅读顺序：

1. `src/websocket.hpp` — 三态结果与校验项清单。
2. `src/websocket.cpp` — 握手、帧解析、长度编码、掩码、UTF-8、Close code。
3. `src/session.*` — 握手超时、分片、控制帧、心跳、读队列。

建议实验：

- 手工计算 `Sec-WebSocket-Accept` 并与服务端对照。
- 把一帧拆成多次 TCP 写入，观察 `NeedMore`。
- 发送未掩码、RSV 非零、非法 opcode、超大控制帧，观察 `ProtocolError` 与断开路径。

验收问题：

- 三态解析为什么比 `bool` 更安全？
- 握手超时、心跳、关闭 deadline 分别解决什么问题？
- 为什么解析放在工作线程而不是 IO 线程？

### 阶段 3：业务组合与认证（2-4 天）

阅读顺序：

1. `src/message.*` — envelope 与错误结构。
2. `src/appserver.hpp` + `appserver.cpp` — 路由表、装配、purge、`syncCaches`。
3. `src/app_util.hpp` — 账号/密码/target 校验。
4. `src/app_auth.cpp` + `src/user_store.*` — 注册、登录、token、登录限流。
5. `src/token_store.*`、`src/redis_token_store.*`、`src/redis_client.*`。

验收问题：

- 未登录门禁放行哪些指令？
- 登录限流为什么按用户名而不是仅按 IP？PBKDF2 并发闸解决什么？
- token 为什么只存摘要？Redis 宕机时免密重连为什么降级？

### 阶段 4：存储与中间件（3-6 天）

阅读顺序：

1. `src/mysql_client.*` — RAII、prepared statement、断线自愈。
2. `src/user_repository.hpp` + `mysql_user_repository.cpp` — 第一个仓储范例。
3. `src/presence_store.hpp` + `online_manager.*` — 在线路由与多端。
4. `src/rate_limiter_store.*`、`src/msg_dedup.*` 及 Redis 版 — 双后端。
5. `src/message_repository.hpp` + `mysql_message_repository.cpp` — 消息状态机与游标事实源。
6. `db/schema.sql` + `db/migrate_*.sql` — 八表与迁移。

验收问题：

- Repository 接口隔离带来了什么好处？(可替换、可单测、业务不依赖容器)
- `messages.id` 与 `messageId` 为什么必须分开？
- 四个限流器如何共享实现、隔离额度桶？

### 阶段 5：功能链路（5-8 天）

按功能依赖顺序阅读：

1. `app_message.cpp` — 单聊发送：幂等 → 限流 → 权限 → 落库 → ACK/离线 → 多端回显。
2. `app_group.cpp` + `group_repository.*` — 群生命周期、单行存储、连续前缀 ACK。
3. `app_contact.cpp` + `app_block.cpp` + `app_settings.cpp` — 联系人、拉黑、隐私。
4. `media_service.cpp` + `object_store.hpp` + `sigv4.cpp` + `minio_object_store.cpp` — 媒体直传。
5. 撤回与会话列表（在 `app_message.cpp`、`mysql_message_repository.cpp`）。

验收问题：

- 拉黑为什么是双向断联且不追溯历史？
- 群消息为什么单行存储 + 水位线，而不是写扩散？
- 媒体为什么要预签名直传而不是服务端中转？

### 阶段 6：可靠投递语义（2-4 天，项目精华）

阅读：

1. `docs/protocol.md` v3.3 变更说明与 ACK 指令表。
2. `docs/superpowers/specs/2026-10-02-delivery-semantics-v2-design.md`。
3. `docs/superpowers/specs/2026-10-02-reliable-delivery-design.md`。
4. `src/app_message.cpp` 的 `onSend/onOfflineAck/pushOfflineMessages`。
5. `src/app_group.cpp` 的 `onGroupOfflineAck`。
6. `src/mysql_message_repository.cpp` 的 `markDelivered`、`fetchGroupMessages`、`fetchMessageCursor`。
7. `src/group_repository.cpp` 的 `advanceDeliveredCursor`。

必须回答：

- off/on 两种模式的行为差异是什么？为什么禁止单/群分开灰度？
- 单聊 PENDING 到 DELIVERED 的完整链路？
- 群 ACK 为什么必须「连续前缀」，空洞如何处理？
- 为什么这是 at-least-once？客户端如何配合去重？

### 阶段 7：测试、运维与部署（2-4 天）

阅读与操作：

1. `CMakeLists.txt` 与 `tests/` — 13 个单测如何组织（纯逻辑源码集复用）。
2. `scripts/ci.sh` — 五步流程与 `--full` 的 ASan 门禁。
3. `scripts/biz_test.py` — 裸 socket 手写 WebSocket 客户端的回归体系。
4. `scripts/bench.py` — 连接与吞吐基线。
5. `metrics.*` 与 `/health`。
6. `docker-compose.yml`、`nginx/im.conf`、`Dockerfile`、`docs/nginx-deploy.md`。

验收问题：

- 业务回归为什么双后端各跑一遍？
- CI 如何保证 sanitizer 零报告？
- Nginx 前置后真实 IP 如何传递？为什么要 `IM_TRUSTED_PROXIES`？

## 5. 完整源码阅读顺序

如果时间有限，只读这一条主线：

1. `src/server_config.hpp`
2. `src/main.cpp` + `src/log.hpp`
3. `src/eventloop.hpp/cpp` + `src/channel.hpp/cpp`
4. `src/eventloop_thread*.*` + `src/threadpool.*`
5. `src/tcpserver.*`
6. `src/connection.*`
7. `src/websocket.*`
8. `src/session.*`
9. `src/message.*`
10. `src/appserver.hpp` + `src/appserver.cpp`
11. `src/app_util.hpp` + `src/app_auth.cpp`
12. `src/message_repository.hpp` + `src/mysql_message_repository.cpp`
13. `src/app_message.cpp`
14. `src/group_repository.*` + `src/app_group.cpp`
15. `src/contact_repository.*` + `src/app_contact.cpp` + `src/app_block.cpp` + `src/app_settings.cpp`
16. `src/media_service.*` + `src/sigv4.*` + `src/minio_object_store.*`
17. `db/schema.sql`
18. `docs/protocol.md`

## 6. 实验清单

1. 从源码计算一次 `Sec-WebSocket-Accept`。
2. 把一帧拆成多次 TCP 写入，观察 `NeedMore`。
3. 发送未掩码 / RSV 非零 / 非法 opcode，观察 `ProtocolError`。
4. 发送 Text + Continuation 分片与超大控制帧。
5. 用慢客户端触发 8MB 写队列与 `SendResult::Overflow`。
6. 重复发送相同 `clientMsgId`，验证幂等不耗配额。
7. 在 `IM_DELIVERY_ACK_REQUIRED=1` 下验证离线补发与 `msg.offline_ack`。
8. 构造群 ACK 空洞，验证连续前缀停止推进。
9. 跑 `media.presign → 直传 → commit → msg.send(image) → media.url` 全链路。
10. 停掉 Redis 验证消息收发不受影响、token 重连降级。
11. 杀掉 MySQL 连接验证断线自愈重试。
12. `IM_SANITIZE=ON` 与 `IM_TSAN=ON` 构建并跑全链路回归。
13. 用 `strace` 观察 `epoll_wait/recv/send/eventfd`。
14. 用 `bench.py` 观察连接数与 RSS 基线。

## 7. 文档与资源索引

| 文档 | 用途 |
|---|---|
| `README.md` | 全貌与部署入口（注意规模与版本口径偏旧） |
| `docs/protocol.md` | **协议唯一事实源**（v3.3） |
| `docs/client_api.md` | 逐指令接口契约 |
| `docs/server-architecture-learning-guide.md` | 架构与源码导读 |
| `docs/client_design.md` | Qt6 客户端设计（头部版本偏旧） |
| `docs/nginx-deploy.md` | 部署与真实 IP 说明 |
| `docs/superpowers/specs/` | 各功能设计规格（为什么/语义） |
| `docs/superpowers/plans/` | 各功能落地计划（怎么做） |
| `db/schema.sql` + `db/migrate_*.sql` | 表结构与幂等迁移 |
| `docs/docs/` | v2.4 过期快照，**忽略** |

阅读功能设计时，建议按时间顺序：联系人（09-05）→ 工程收尾（09-06）→ 群聊（09-07）→ 群管理（09-16）→ 媒体（10-01）→ 撤回（10-02）→ 可靠投递（10-02），最后精读可靠投递两份文档。

## 8. 推荐学习节奏

| 周次 | 内容 | 产出 |
|---|---|---|
| 第 1 周 | 阶段 0 + 阶段 1 | 能独立起服务、跑 CI、讲清线程模型 |
| 第 2 周 | 阶段 2 | 能手工构造/解析 WebSocket 帧并解释校验 |
| 第 3 周 | 阶段 3 + 阶段 4 | 能画出一次登录的完整数据流；能写仓储单测 |
| 第 4 周 | 阶段 5 | 能画出单聊/群聊/媒体三条链路 |
| 第 5 周 | 阶段 6 + 阶段 7 | 能解释双 ACK 语义并跑通双后端回归 |

## 9. 后续演进方向

当前未实现或可继续完善的方向：

1. **多实例水平扩展**：Presence 与限流状态迁到 Redis 共享，引入消息队列做跨实例投递。
2. **TLS 网关**：启用 `docs/nginx-deploy.md` 方案 B，对外 `wss://`。
3. **群已读**：`group_members.last_read_ts` + `group.read/read_notify`（协议已预留占位）。
4. **消息漫游**：跨端历史与已读统一。
5. **文档债务清理**：README 规模/版本口径、`.env.example` 缺失、`appserver.cpp` 路由数注释、`client_design.md` 版本头、`docs/docs/` 快照删除。
6. **观测增强**：把 `/health` 接入 Prometheus 类采集，增加延迟直方图。
7. **压测深化**：多实例、慢客户端、故障注入场景的基线。

## 10. 总自测题

1. 项目从「纯内存学习版」到 v3.3 经历了哪些关键阶段？
2. 一次 `msg.send` 从 TCP 字节到对端 `msg.recv` 经过哪些线程与存储？
3. 三态帧解析、背压三态、双 ACK 三者分别解决什么问题？
4. 为什么状态与语义要先于基础设施演进？
5. 如何在不改变协议的前提下把内存实现替换为 Redis/MySQL？
6. 如果明天要做多实例，需要先改造哪三处（提示：Presence、限流、投递路由）？
