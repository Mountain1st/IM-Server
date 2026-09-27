# IM 即时通讯服务端（C++17）

单进程高性能 WebSocket IM 服务端：手写 Reactor 网络层与 RFC 6455 协议栈，
MySQL 持久化 + Redis 可选双后端，覆盖注册登录、单聊、联系人、群聊（含群管理）、
离线消息、多端一致全链路。面向学习与生产化实践设计——功能复杂度可控，
正确性、安全边界与测试标准按生产系统要求。

## 特性

- **网络**：主从 Reactor（epoll ET + eventfd + timerfd），IPv6 双栈，单 IP 连接限流，多层背压（2MB 帧 / 8MB 队列 / 8MB 写缓冲）
- **协议**：手写 WebSocket 帧解析与握手校验（掩码/RSV/opcode/UTF-8/Close code），JSON 信封协议（v2.2）
- **认证**：PBKDF2-HMAC-SHA256（10 万迭代 + 独立盐），token 摘要化存储，登录限流（防账号枚举与洗表绕过）
- **单聊**：account 定位、消息状态机（offline→delivered→read）、非破坏性离线补发（确认制）、历史、已读回执
- **联系人**：搜索/请求/接受/删除，三开关隐私（临时对话/可搜索/可添加），列表恒含自己
- **群聊**：建群/拉人/退群/历史/群发（单行存储 + 每成员水位线补发），群管理（踢人/转让/解散/改名/公告）
- **多端**：一账号多设备同时在线，自发消息回显 + 已读同步（msg.read_notify）
- **存储**：MySQL 六表全参数化 SQL；Redis 可选（token/限流/幂等/在线状态，调用带超时且锁外执行——Redis 宕机不影响消息收发）
- **可靠性**：clientMsgId 幂等、滑动窗口限流、离线消息 TTL 清理（7 天）、优雅停机

## 快速开始

```bash
# 方式一：Docker 全家桶（推荐）
docker compose up -d --build

# 方式二：本地构建（需 libmysqlclient-dev，可选 libhiredis-dev）
cmake -B build && cmake --build build -j4
./build/imserver 9191 2 3 500    # 端口/IO线程/工作线程/最大连接
```

配置经环境变量注入（`IM_MYSQL_*`、`IM_REDIS_*`、`IM_OFFLINE_PURGE_INTERVAL`、
`IM_MAX_PER_IP_CONN`），见协议文档 §10。云服务器（2C2G）部署调优清单见 §10.4。

## 测试

```bash
ctest --test-dir build                 # 7 个单元测试
bash scripts/test_biz.sh               # 205 个业务回归用例（WebSocket 全链路）
IM_REDIS_ENABLED=1 IM_REDIS_PORT=6379 bash scripts/test_biz.sh   # Redis 模式

# Sanitizer（已通过 ASan/UBSan/LSan/TSan 全链路，修复过 2 处线程身份发布竞争）
cmake -B build-asan -DIM_SANITIZE=ON && cmake --build build-asan -j4
IM_SERVER_BIN=./build-asan/imserver bash scripts/test_biz.sh     # ASan 全链路
cmake -B build-tsan -DIM_TSAN=ON && cmake --build build-tsan -j4
IM_SERVER_BIN=./build-tsan/imserver bash scripts/test_biz.sh     # TSan 全链路
```

## 架构（一图）

```text
TCP → Connection(IO线程,epoll ET,背压) → Session(握手/心跳/8MB队列)
    → ThreadPool(工作线程) → AppServer 路由 → 域处理器(auth/message/contact/group)
    → Repository(接口隔离) → MySQL(六表) / Redis(可选四类)
```

三条值得聊的技术决策：

1. **群消息单行存储 + 水位线**而非写扩散：50 人群一条 INSERT 而非 50 条，
   离线补发按每成员水位线读时过滤——写少读可控的权衡
2. **Redis 只做"重启不失忆"**：token/限流/幂等/在线状态可选上 Redis 且全部带超时+锁外化，
   Redis 宕机时消息收发零影响，只降级免密重连与在线显示
3. **测试金字塔**：7 个协议/逻辑单测 + 205 个真实 WebSocket 业务用例（双后端模式各跑一遍），
   每次功能迭代 TDD 红绿循环（如 TTL 清理的单位 bug 即由边界用例回归发现）

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/protocol.md](docs/protocol.md) | 协议规范（v2.2）：指令/事件/错误码/部署 |
| [docs/client_api.md](docs/client_api.md) | 客户端接入标准（逐指令请求/响应/错误码） |
| [docs/client_design.md](docs/client_design.md) | Windows 客户端设计与接口（Qt6） |
| [docs/server-architecture-learning-guide.md](docs/server-architecture-learning-guide.md) | 架构与源码学习指南 |
| [docs/improvement-plan-and-learning-roadmap.md](docs/improvement-plan-and-learning-roadmap.md) | 改进计划与学习路线（含当前状态表） |

## 规模与边界

- 代码 ~7500 行（46 文件），7 单测 + 205 业务用例，MySQL 六表，Redis 四类键
- 单机容量（2C2G 调优后）：约 2000-5000 在线连接、500-1000 msg/s（瓶颈为仓库层单 MySQL 连接）
- 明确非目标：多实例水平扩展、消息队列、TLS 网关（见改进计划文档）
