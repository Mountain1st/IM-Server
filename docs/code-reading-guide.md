# 源码阅读入口与学习模块（v3.3）

本文是速查版阅读入口：从哪里开始、按什么顺序读、每个模块看什么。完整讲解见 `docs/server-architecture-learning-guide.md`，阶段化路线见 `docs/improvement-plan-and-learning-roadmap.md`。

> **基准：** `HEAD` 为协议 v3.3（2026-10-03）。协议事实以 `docs/protocol.md` 为准；`docs/docs/` 为过期快照，忽略。

## 1. 从哪里开始

不要从 Reactor 或 WebSocket 直接切入，按「全局地图 → 启动骨架 → 一条请求链路」进入：

| 顺序 | 入口 | 目的 |
|---|---|---|
| 第一站 | `src/server_config.hpp` | 全项目地图：资源上限、超时、开关（含 `deliveryAckRequired()`） |
| 第二站 | `src/main.cpp` + `src/log.hpp` | 启动参数/环境变量、信号、停机四步、15 秒看门狗 |
| 第三站 | 跟一条 `msg.send` 全链路 | 串起网络、协议、业务、存储、写回 |

一条请求的完整调用链（按线程标注）：

```text
[IO 线程]   tcpserver.cpp accept
            connection.cpp handleRead（ET 读到 EAGAIN）
            session.cpp enqueueAndDrain（入 8MB 队列）
[工作线程]  session.cpp drain -> onMessage -> FrameParser -> processFrames
            session.cpp handle -> handleData -> textHandler
            message.cpp parseEnvelope
            appserver.cpp onEnvelope -> routes_ 查表
            app_message.cpp onSend（幂等 -> 限流 -> 权限 -> 落库 -> ACK/离线）
            mysql_message_repository.cpp saveMessage
[IO 线程]   session.cpp reply -> connection.cpp send（写队列 + eventfd）
            eventloop.cpp flushWrite -> 非阻塞 send
```

## 2. 学习路径（7 个阶段）

| 阶段 | 目标 | 阅读顺序 | 验收问题 |
|---|---|---|---|
| 0 跑起来 | 建立系统体感 | `README.md` → `docs/protocol.md` → `compose.dev.yaml` + `db/schema.sql` → `scripts/ci.sh` | 能起服务、跑回归、说清一次 `msg.send` 经过哪些组件 |
| 1 网络核心 | 掌握 Reactor 与线程 | `server_config.hpp` → `main.cpp` + `log.hpp` → `eventloop.*` + `channel.*` → `eventloop_thread*.*` + `threadpool.*` → `tcpserver.*` → `connection.*` | 最少连接分配、ET 读到 `EAGAIN`、8MB 背压与 `SendResult` 三态 |
| 2 WebSocket | 协议栈核心 | `websocket.hpp` → `websocket.cpp`（握手/三态解析/掩码/UTF-8/Close code）→ `session.*`（超时/分片/心跳/读队列） | 手工计算 Accept、解释 `NeedMore/Complete/ProtocolError` |
| 3 业务组合 | 路由与认证 | `message.*` → `appserver.*` → `app_util.hpp` → `app_auth.cpp` + `user_store.*` | 未登录门禁、PBKDF2 并发闸、token 摘要化 |
| 4 存储中间件 | 持久化与双后端 | `mysql_client.*` → `user_repository` + `mysql_user_repository` → `presence_store`/`online_manager` → 限流/幂等双后端 → `message_repository.hpp` + `mysql_message_repository.cpp` → `db/schema.sql` | `messages.id` 与 `messageId` 的区别、Redis 降级边界 |
| 5 功能链路 | 业务全链路 | `app_message.cpp` → `app_group.cpp` + `group_repository` → `app_contact/block/settings` → `media_service` + `sigv4` + `minio_object_store` | 画出单聊、群聊、媒体三条链路 |
| 6 可靠性 | 项目精华 | `protocol.md` v3.3 → `specs/2026-10-02-delivery-semantics-v2-design.md` → `onSend/onOfflineAck/pushOfflineMessages` → `advanceDeliveredCursor` | off/on 模式、连续前缀为何不能跳空洞、at-least-once |
| 7 工程运维 | 交付与质量 | `CMakeLists.txt` + `tests/` → `ci.sh` → `biz_test.py`/`bench.py` → `metrics.*` + `/health` → `docker-compose.yml` + `nginx/im.conf` | 双后端回归、Sanitizer 门禁、真实 IP 链路 |

## 3. 学习模块划分

| 模块 | 范围 | 关键文件 | 核心技术点 |
|---|---|---|---|
| A 网络与并发 | Reactor、线程、连接 | `eventloop.*`、`channel.*`、`tcpserver.*`、`connection.*`、`threadpool.*`、`eventloop_thread*.*`、`session.*` | epoll ET、eventfd、timerfd、背压、写完关闭、Session 串行化 |
| B 协议 | WebSocket 与信封 | `websocket.*`、`message.*`、`docs/protocol.md` | RFC 6455、三态解析、分片、控制帧、JSON envelope |
| C 业务 | 路由与各业务域 | `appserver.*`、`app_auth.cpp`、`app_message.cpp`、`app_contact.cpp`、`app_group.cpp`、`app_block.cpp`、`app_settings.cpp`、`app_util.hpp` | 哈希路由、域拆分、认证、消息编排 |
| D 存储 | 持久化与双后端 | `mysql_client.*`、`*_repository.*`、`mysql_*_repository.*`、`redis_*`、`db/schema.sql`、`db/migrate_*.sql` | Repository 模式、prepared statement、断线自愈、Redis 键设计 |
| E 可靠性 | 投递语义与防护 | `app_message.cpp`、`app_group.cpp`、`mysql_message_repository.cpp`、`group_repository.cpp`、`rate_limiter_store.*`、`msg_dedup.*` | 双 ACK、连续前缀游标、幂等、四层限流、TTL/保留期 |
| F 媒体 | 对象存储直传 | `media_service.*`、`media_repository.hpp`、`mysql_media_repository.*`、`object_store.hpp`、`sigv4.*`、`minio_object_store.*` | 预签名、SigV4、HEAD 校验、媒体 GC |
| G 工程 | 质量与部署 | `log.hpp`、`metrics.*`、`CMakeLists.txt`、`tests/`、`scripts/`、`docker-compose.yml`、`nginx/im.conf` | 单测/回归/压测、Sanitizer、CI、Nginx、可观测性 |

## 4. 建议首周安排

| 时间 | 内容 | 产出 |
|---|---|---|
| 第 1 天 | 阶段 0：起依赖、跑 `scripts/ci.sh`、看 `/health` 输出 | 环境可用，能说清组件全貌 |
| 第 2-3 天 | 阶段 1：网络核心 | 画出线程模型与背压路径 |
| 第 4-5 天 | 阶段 2：WebSocket，做帧构造实验 | 能手工构造/解析帧并解释校验 |
| 第 6-7 天 | 阶段 3-4：走通登录到单聊完整链路 | 能画出一次请求跨线程/跨存储的时序 |

## 5. 关联文档

| 文档 | 用途 |
|---|---|
| `docs/server-architecture-learning-guide.md` | 架构细节与逐模块讲解 |
| `docs/improvement-plan-and-learning-roadmap.md` | 前置知识、阶段路线、实验清单 |
| `docs/protocol.md` | 协议唯一事实源（v3.3） |
| `docs/client_api.md` | 逐指令接口契约 |
| `docs/nginx-deploy.md` | 部署与真实 IP |
| `docs/superpowers/specs/` | 各功能设计规格（为什么/语义） |
