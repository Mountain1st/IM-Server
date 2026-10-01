# IM 即时通讯服务端（C++17）

单进程高性能 WebSocket IM 服务端：手写 Reactor 网络层与 RFC 6455 协议栈，
MySQL 持久化 + Redis 可选双后端，覆盖注册登录、单聊、联系人、群聊（含群管理）、
离线消息、多端一致全链路。面向学习与生产化实践设计——功能复杂度可控，
正确性、安全边界与测试标准按生产系统要求。

## 特性

- **网络**：主从 Reactor（epoll ET + eventfd + timerfd），IPv6 双栈，单 IP 连接限流，多层背压（2MB 帧 / 8MB 队列 / 8MB 写缓冲）
- **协议**：手写 WebSocket 帧解析与握手校验（掩码/RSV/opcode/UTF-8/Close code），JSON 信封协议（v2.4）
- **认证**：PBKDF2-HMAC-SHA256（10 万迭代 + 独立盐），token 摘要化存储，登录限流（防账号枚举与洗表绕过）
- **单聊**：account 定位、消息状态机（offline→delivered→read）、非破坏性离线补发（确认制）、历史、已读回执
- **图片消息**：`msgType: text/image` 双类型（ack 不带 msgType）；image 信封 base64 上限 1.5MB（PNG/JPEG/GIF/WebP，拒 SVG，维度任一边 ≤4096）；HMAC-SHA256 对象键落盘 `/uploads/<hex>.<ext>`（DB 存 URL + msg_type 列）；**双重限流**（通用发送限流 + 按用户 10 张/分钟）与解码**并发闸 2**；落盘 7 天过期即 404，Nginx 只读 alias 托管（`max-age=86400`，1 天）
- **联系人**：搜索/请求/接受/删除，三开关隐私（临时对话/可搜索/可添加），列表恒含自己
- **拉黑**：双向断联（分方向 403 文案、好友请求模糊防探测、自动删联系人、群聊不受影响）
- **群聊**：建群/拉人/退群/历史/群发（单行存储 + 每成员水位线补发），群管理（踢人/转让/解散/改名/公告）
- **多端**：一账号多设备同时在线，自发消息回显 + 已读同步（msg.read_notify）
- **存储**：MySQL 七表全参数化 SQL；Redis 可选（token/限流/幂等/在线状态，调用带超时且锁外执行——Redis 宕机不影响消息收发）
- **可靠性**：clientMsgId 幂等、滑动窗口限流（发送/查询/注册三层）、离线消息
  TTL 清理（7 天）、优雅停机（Close 1001 通知 + 工作队列排水）；四缓存低频比对
  自愈（联系人/群成员/用户/拉黑）；MySQL/Redis 断线自愈（运行期无需重启）；
  PBKDF2 并发闸 + 登录拆锁（防慢哈希阻塞热路径）；compose restart 兜底
- **工程**：`-Wall -Wextra` 零警告基线；ASan/UBSan/LSan/TSan 全矩阵通过
- **部署**：Docker Compose 四件套（mysql+redis+imserver+**nginx 反代**）；
  `X-Forwarded-For` 真实 IP 信任链、TLS（wss）终结位、日志按大小轮转；
  时间显示统一北京时间 UTC+8（日志固定偏移不依赖容器时区，MySQL/nginx 容器 TZ）

## 快速开始

```bash
# 方式一：Docker 全家桶（推荐，部署标准配置）
cp .env.example .env 2>/dev/null; echo "IM_MYSQL_PASSWORD=改成强密码" > .env
docker compose up -d --build   # mysql+redis+imserver+nginx（8080 反代入口）

# 方式二：本地构建（需 libmysqlclient-dev，可选 libhiredis-dev）
cmake -B build && cmake --build build -j4
./build/imserver 9191 2 3 50    # 端口/IO线程/工作线程/最大连接
```

部署标准（按远程生产机固化，后续改进以此为准）：连接上限 50（2C2G）、
imserver 仅绑 `127.0.0.1:9191`（对外唯一入口 Nginx 8080）、
`IM_TRUSTED_PROXIES=网桥 IP`（换机器核对网段）、MySQL buffer pool 128M、
密码经 `.env` 注入（compose 自动读取）。

配置经环境变量注入（`IM_MYSQL_*`、`IM_REDIS_*`、`IM_OFFLINE_PURGE_INTERVAL`、
`IM_MAX_PER_IP_CONN`），见协议文档 §10。云服务器（2C2G）部署调优清单见 §10.4。

## 测试

```bash
bash scripts/ci.sh                     # 本地 CI 一键：构建+单测+双模式回归（--full 加 ASan）
ctest --test-dir build                 # 9 个单元测试
bash scripts/test_biz.sh               # 246 个业务回归用例（含断线自愈、整库宕机恢复、唯一性防线、v2.4 拉黑 20 用例）
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
    → ThreadPool(工作线程) → AppServer 哈希路由(O(1)查表) → 域处理器(auth/message/contact/group)
    → Repository(接口隔离) → MySQL(六表) / Redis(可选四类)
IO 线程分配：最少连接优先（least-connections）；停机：停接入→Close(1001)→队列排水→兜底
```

三条值得聊的技术决策：

1. **群消息单行存储 + 水位线**而非写扩散：50 人群一条 INSERT 而非 50 条，
   离线补发按每成员水位线读时过滤——写少读可控的权衡
2. **Redis 只做"重启不失忆"**：token/限流/幂等/在线状态可选上 Redis 且全部带超时+锁外化，
   Redis 宕机时消息收发零影响，只降级免密重连与在线显示
3. **测试金字塔**：9 个协议/逻辑单测 + 217 个真实 WebSocket 业务用例（双后端模式各跑一遍），
   每次功能迭代 TDD 红绿循环（如 TTL 清理的单位 bug 即由边界用例回归发现）

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/protocol.md](docs/protocol.md) | 协议规范（v2.2）：指令/事件/错误码/部署 |
| [docs/client_api.md](docs/client_api.md) | 客户端接入标准（逐指令请求/响应/错误码） |
| [docs/client_design.md](docs/client_design.md) | Windows 客户端设计与接口（Qt6） |
| [docs/server-architecture-learning-guide.md](docs/server-architecture-learning-guide.md) | 架构与源码学习指南 |
| [docs/improvement-plan-and-learning-roadmap.md](docs/improvement-plan-and-learning-roadmap.md) | 改进计划与学习路线（含当前状态表） |

## 规模与边界（含实测基线）

- 代码 ~7600 行（47 文件），8 单测 + 210 业务用例，MySQL 六表，Redis 四类键
- **实测基线**（`scripts/bench.py`，2026-09-29，本地 3 工作线程 + 临时 MySQL）：
  - 连接：**2000 并发连接 30 秒全保持**，建连速率 **1300+/s**；服务端 RSS 仅 **23MB**
  - 吞吐：40 用户并发 **43 msg/s、p50 25ms / p99 37ms**（受单用户限流 30 条/10s 的
    生产语义约束，非服务端上限；单条落库 ~3ms，单 MySQL 连接串行理论容量约
    300-500 msg/s，扩容路径见改进计划 C 区）
- 明确非目标：多实例水平扩展、消息队列、TLS 网关、多端消息漫游（见改进计划文档）
