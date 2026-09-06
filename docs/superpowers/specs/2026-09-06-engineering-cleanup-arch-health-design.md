# 工程收尾 + 架构健康 设计规格（v1.8.1 后）

日期：2026-09-06
状态：三节设计已获用户批准；待用户审查书面规格

## 背景与范围

项目处于"单机功能完备、工程化收尾前夜"：核心链路已打通（v1.8.1），回归 105/105 + CTest 4/4，代码零 TODO。
本次规格覆盖**阶段1（工程收尾）+ 阶段2（架构健康）**；**群聊为后续独立规格，不在本次范围**；**CI 暂缓**（无 git remote，等有远端再说）。

## 已确认决策

| # | 问题 | 决策 |
|---|---|---|
| Q1 | CI 跑在哪里 | C：暂缓，等有远端再说 |
| Q2 | git 归档范围 | 只跟踪 src + CMakeLists（后经 Q4/Q7 扩大） |
| Q3 | 已在库的 9 个 src 外文件 | B：保留现状，今后只提交 src/CMakeLists（后经 Q4/Q7 扩大） |
| Q4 | compose 冲突 | A：修；本地 compose.yaml 向 docker-compose.yml 对齐；跟踪范围扩大到 docker/mysql/redis 配置文件 |
| Q5 | 日志框架 | A：自研轻量 header-only logger |
| Q6 | appserver 拆分粒度 | A：按业务域拆 5 文件 |
| Q7 | tests/ 是否跟踪 | A：纳入跟踪 |
| Q8 | 新单测范围 | B：logger + 可独立测试的纯函数（resolveTarget） |
| 方案 | 推进顺序 | A：按依赖顺序连续推进（git→compose→logger→拆分→单测→minor→回归） |

**最终 git 跟踪政策**（Q2/Q3/Q4/Q7 演进结果）：src/ + CMakeLists.txt + 已入库文件（db/、docs/*.md、specs/plans、biz_test.py）+ docker 配置（Dockerfile、compose.yaml、docker-compose.yml、.dockerignore、.gitignore）+ tests/。其余（client/、third_party/、.vscode/、`__pycache__/`、build 产物、docs/docs/ 等）保持库外。

## §1 工程收尾

### Git 跟踪扩围
- 新增入库：`Dockerfile`、`compose.yaml`、`docker-compose.yml`、`.dockerignore`、`.gitignore`、`tests/`（5 文件：4 单测 + test_util.hpp）
- 保持库外：`client/`、`third_party/`、`.vscode/`、`__pycache__/`、build 产物、`docs/docs/` 等
- 顺序：先提交 `.gitignore`，再加其余文件

### Compose 对齐
- `compose.yaml`（本地开发）向 `docker-compose.yml`（部署）看齐：mysql `8.0→8.4`、`MYSQL_DATABASE IM_reactor_db→IMS_reactor_db`、`MYSQL_USER IM_jack→IMS_jack`（与 db_env.sh/biz_test.py 期望一致）；redis `7→8-alpine`，保留 `6380` 映射供本地测试；保留容器名与端口
- 两文件加角色注释（dev 仅 infra vs deploy 全栈）
- 版本升级需重建本地卷（`down -v`；库内现为测试数据，可接受）

### Logger（`src/log.hpp`，自研 header-only）
- `LOG_D/I/W/E` 宏：时间戳 + 线程 id + 文件行号；线程安全；stderr + 可选文件双落；`Log::init()` 从环境变量读配置（`IM_LOG_FILE` 为空=仅stderr，`IM_LOG_LEVEL` 缺省 INFO），沿用项目 env 配置习惯
- v1 不做轮转（YAGNI，后续加）；迁移 ~96 处调用（cout→INFO，cerr→ERROR，逐处判断）；保留 `[tag]` 前缀习惯
- 单测 `tests/log_test.cpp`（级别过滤/文件落盘/格式）

## §2 架构健康

### 拆分（appserver.cpp 938行 → 5 文件，纯搬移零行为变更）
- `appserver`：核心（生命周期 start/stop/run、onEnvelope 路由、连接管理、成员声明）
- `app_auth`：register/login/token/logout
- `app_message`：send/offline_ack/history/read/create + pushOfflineMessages + nextMessageId
- `app_contact`：search/contact 7 个 handler + pushPendingRequests
- `app_settings`：settings get/set
- 共享 AppServer 成员（hpp 声明集中保留）；CMakeLists 加 4 个 cpp；用 105 项 biz 回归钉住"行为不变"

### 单测
- `resolveTarget`/`validAccount`（现 appserver.cpp:37/82 匿名 namespace）抽至 header-only 的 `src/app_util.hpp`（内含 message.hpp/user_store.hpp/server_config.hpp 引用）使其可测；单测直接 include 该头 + fake UserRepository，不碰 MySQL
- `tests/log_test.cpp` + `tests/target_parse_test.cpp`（后者用内存 fake UserRepository，不碰 MySQL；覆盖 account 判定/userId 兼容/非法→-1/不存在→-2）
- handler 层不造 mock（已有 biz 回归覆盖）

### Minor 清理（6 项全修，src 内小改）
1. `fetchSettings` 区分 MYSQL_NO_DATA（不存在→1）与真实错误（→2）
2. `searchUsers` 转义 LIKE 通配符 `%/_` + keyword 长度上限 64（复用 `kLoginAccountMaxLen`）
3. 删 `onContactRequest` 未用变量 account
4. `contact.request` 在线推送 ts 改取 DB created_at（与离线补发同源，消秒级偏差）
5. `updateSettings` void→int，失败回 500（替代静默假成功）
6. 非联系人消息的 `fetchSettings` 加内存缓存（缓存在 `UserStore` 内、复用其现有 mutex，`updateSettings` 时失效），消逐条 DB 查询

## §3 验证与提交策略

### 提交顺序（每步独立提交，沿用中文 commit 规范）
1. `.gitignore` + 跟踪扩围
2. compose 对齐（含 `down -v` 重建说明）
3. logger（`src/log.hpp` + 调用点迁移 + `log_test`）
4. appserver 拆 5 文件（纯搬移）
5. `target_parse_test` + helper 抽取
6. minor 6 项逐项修
7. 全量回归验证（非提交步骤）

### 验证门禁（每步必过）
- `cmake --build build` 零 warning、`ctest` 4→6 项全过
- `test_biz.sh` 内存 105/105、`IM_REDIS_ENABLED=1` Redis 模式 105/105、`test_origin.sh`
- 拆分步骤额外要求：拆分前后 biz 结果逐项一致（行为零变更证据）

### 范围声明
- 本次不改协议（logger/拆分/单测均不触 wire 格式），`protocol.md`/`client_api.md` 无需更新
- 群聊不在本次范围
