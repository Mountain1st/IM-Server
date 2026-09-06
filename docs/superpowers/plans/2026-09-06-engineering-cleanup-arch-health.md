# 工程收尾 + 架构健康 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 落地 git 跟踪扩围、compose 对齐、自研日志框架、appserver 按域拆 5 文件、单测扩展到 logger 与目标解析、6 项 minor 清理，全程回归门禁通过。

**架构：** 严格按依赖顺序推进（git→compose→logger→helper 抽取→拆分→单测→minor→回归）；logger 先于拆分避免二次触碰；helper 先抽取再拆分（拆分文件依赖其跨 TU 可见）；行为零变更由 105 项 biz 回归钉住。

**技术栈：** C++17、CMake、自研 header-only 日志、MySQL、Docker Compose、现有 TEST/CHECK 单测宏。

**规格：** `docs/superpowers/specs/2026-09-06-engineering-cleanup-arch-health-design.md`（未提交，按 git 政策保持）

**测试基线：** `cmake --build build` 零 warning、`ctest` 4/4、`bash scripts/test_biz.sh` 内存 105/105、`IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380 bash scripts/test_biz.sh` Redis 105/105、`bash scripts/test_origin.sh`。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `.gitignore`（改） | 追加 `scripts/__pycache__/`、`__pycache__/`（build/、*.log 已有） |
| `Dockerfile`、`compose.yaml`、`docker-compose.yml`、`.dockerignore`、`tests/`（新增入库） | 部署与测试资产进版本控制 |
| `compose.yaml`（改） | 向 docker-compose.yml 对齐：mysql 8.4、`IMS_jack`/`IMS_reactor_db`、redis 8-alpine，保留 6380/3306 映射与容器名 |
| `src/log.hpp`（新增） | 自研 header-only 日志：LOG_D/I/W/E，时间戳+线程id+文件行号，stderr+文件双落，`IM_LOG_FILE`/`IM_LOG_LEVEL` 配置 |
| `src/*.cpp` 12 文件（改） | ~96 处 cout/cerr → LOG_* 迁移 |
| `src/main.cpp`（改） | 启动时 `Log::init()` |
| `src/app_util.hpp`（新增） | validAccount/validPassword/resolveTarget + k 别名（原 appserver.cpp 匿名 namespace，header-only inline） |
| `src/app_auth.cpp`、`src/app_message.cpp`、`src/app_contact.cpp`、`src/app_settings.cpp`（新增） | 业务 handler 纯搬移；`appserver.cpp` 留核心 |
| `CMakeLists.txt`（改） | 加 4 个拆分 cpp；注册 `log_test`、`target_parse_test`；UNIT_TEST_SOURCES 加 `user_store.cpp`、`token_store.cpp` |
| `tests/log_test.cpp`、`tests/target_parse_test.cpp`（新增） | 新单测（TEST/CHECK 宏） |
| `src/mysql_user_repository.cpp`、`src/user_store.*`、`src/appserver.cpp`、`src/contact_repository.*`（改） | minor 6 项 |

---

### 任务 1：.gitignore 与跟踪扩围

**文件：**
- 修改：`.gitignore`
- 新增入库（git add）：`Dockerfile`、`compose.yaml`、`docker-compose.yml`、`.dockerignore`、`tests/`

- [ ] **步骤 1：.gitignore 追加忽略项**

`.gitignore` 当前已有 `build/`、`build-*/`、`server.log`、`*.log`、`test_client`、`imclient`、`imserver`。末尾追加：
```
scripts/__pycache__/
__pycache__/
```

- [ ] **步骤 2：验证忽略生效**

运行：`git status --short | grep -E '__pycache__|/build' || echo CLEAN`
预期：`CLEAN`（无 build 产物与 pycache 显示；`.vscode/` 仍会显示，属预期，政策决定保持库外）

- [ ] **步骤 3：分批入库并提交**

运行：
```bash
git add .gitignore Dockerfile compose.yaml docker-compose.yml .dockerignore tests/
git status --short | head -20
```
预期：暂存区仅含上述文件（含 `tests/` 下 5 文件：4 单测 + test_util.hpp），无 `client/`、`third_party/`、build 产物。

运行：
```bash
git commit -m "chore: 跟踪扩围（docker/compose 配置与 tests 单测入库）"
```
预期：提交成功，`git log --oneline -1` 显示该提交。

---

### 任务 2：compose 对齐

**文件：**
- 修改：`compose.yaml`（本地开发向内部署看齐）
- 修改：`docker-compose.yml`（仅加角色注释，不改动服务定义）

- [ ] **步骤 1：改写 compose.yaml**

将 `compose.yaml` 改为以下完整内容（mysql 8.4 + `IMS_jack`/`IMS_reactor_db`，与 `db_env.sh`/biz_test 期望一致；redis 8-alpine 保留 6380 映射）：
```yaml
# 本地开发环境：仅 infra（mysql + redis），imserver 本地原生运行直连 127.0.0.1。
# 账号库名与 docker-compose.yml（内部署）保持一致：IMS_jack / IMS_reactor_db。
# 用法: docker compose -f compose.yaml up -d
# 注意：mysql 8.0→8.4 升级需重建卷：docker compose -f compose.yaml down -v && docker compose -f compose.yaml up -d
services:
  redis:
    image: redis:8-alpine
    container_name: ims-redis
    restart: unless-stopped
    ports:
      - "127.0.0.1:6380:6379"
    volumes:
      - redis-data:/data

  mysql:
    image: mysql:8.4
    container_name: ims-mysql
    restart: unless-stopped
    environment:
      MYSQL_ROOT_PASSWORD: 520010
      MYSQL_DATABASE: IMS_reactor_db
      MYSQL_USER: IMS_jack
      MYSQL_PASSWORD: 520010
    ports:
      - "127.0.0.1:3306:3306"
    volumes:
      - mysql-data:/var/lib/mysql

volumes:
  redis-data:
  mysql-data:
```

- [ ] **步骤 2：docker-compose.yml 加角色注释**

在文件首行注释下追加一行（不改动任何服务定义）：
```yaml
# 角色：内部署全栈（mysql + redis + imserver）；本地开发仅 infra 请用 compose.yaml。
```

- [ ] **步骤 3：校验 YAML 合法**

运行：`docker compose -f compose.yaml config > /dev/null && echo COMPOSE_OK`
预期：`COMPOSE_OK`（config 校验不需 daemon）。

- [ ] **步骤 4：Commit**

```bash
git add compose.yaml docker-compose.yml
git commit -m "chore: 本地 compose 向内部署对齐（mysql 8.4/IMS_jack/IMS_reactor_db）"
```
预期：提交成功。

---

### 任务 3：自研日志框架 + 单测（TDD）

**文件：**
- 创建：`src/log.hpp`
- 创建：`tests/log_test.cpp`
- 修改：`CMakeLists.txt`（注册 log_test）
- 测试：`tests/log_test.cpp`

- [ ] **步骤 1：编写失败的测试**

创建 `tests/log_test.cpp`（完整内容）：
```cpp
#include "test_util.hpp"

#include "log.hpp"

#include <string>

using namespace im;

TEST(log_level_filter) {
  log::Log::setLevel(log::Level::WARN);
  FILE* f = tmpfile();
  log::Log::setFile(f);
  log::Log::write(log::Level::INFO, "t.cpp", 1, "hidden-msg");
  log::Log::write(log::Level::ERROR, "t.cpp", 2, "shown-msg");
  std::fflush(f);
  std::rewind(f);
  char buf[1024] = {0};
  size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::string s(buf, n);
  CHECK(s.find("hidden-msg") == std::string::npos);
  CHECK(s.find("shown-msg") != std::string::npos);
  log::Log::shutdown();
  log::Log::setLevel(log::Level::INFO);
}

TEST(log_format_fields) {
  FILE* f = tmpfile();
  log::Log::setFile(f);
  log::Log::write(log::Level::WARN, "myfile.cpp", 42, "hello");
  std::fflush(f);
  std::rewind(f);
  char buf[1024] = {0};
  size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::string s(buf, n);
  CHECK(s.find("WARN") != std::string::npos);
  CHECK(s.find("myfile.cpp:42") != std::string::npos);
  CHECK(s.find("hello") != std::string::npos);
  log::Log::shutdown();
}

TEST(log_macros_emit) {
  FILE* f = tmpfile();
  log::Log::setFile(f);
  LOG_I("num=" << 7);
  LOG_E(std::string("oops"));
  std::fflush(f);
  std::rewind(f);
  char buf[1024] = {0};
  size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::string s(buf, n);
  CHECK(s.find("num=7") != std::string::npos);
  CHECK(s.find("oops") != std::string::npos);
  log::Log::shutdown();
}

int main() {
  RUN(log_level_filter);
  RUN(log_format_fields);
  RUN(log_macros_emit);
  std::printf("log_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤 2：注册测试并运行验证失败**

`CMakeLists.txt` 在 `im_add_unit_test(rate_limiter_test tests/rate_limiter_test.cpp)` 后追加：
```cmake
im_add_unit_test(log_test tests/log_test.cpp)
```
运行：`cmake -S . -B build > /dev/null && cmake --build build --target log_test 2>&1 | grep -iE 'error' | head -5`
预期：报错（`src/log.hpp` 不存在，`#include "log.hpp"` 失败）——确认测试先失败。

- [ ] **步骤 3：编写 src/log.hpp（完整内容）**

```cpp
#pragma once
// 轻量日志（header-only，自研）：级别 + 时间戳 + 线程 id + 文件行号，线程安全。
// 配置经环境变量：IM_LOG_FILE 为空 = 仅 stderr；IM_LOG_LEVEL 缺省 INFO。
// v1 不做轮转（运维侧截断），后续按需加。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace im {
namespace log {

enum class Level { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Log {
 public:
  // 读 IM_LOG_FILE / IM_LOG_LEVEL 并初始化（main 启动时调用一次）。
  static void init();
  // 单测用：直接设级别 / 输出文件。
  static void setLevel(Level lv);
  static void setFile(FILE* f);
  // 关闭文件输出并回到 stderr（单测清理用）。
  static void shutdown();
  static void write(Level lv, const char* file, int line, const std::string& msg);

 private:
  static const char* levelName(Level lv);
  static std::mutex& mutex();
  static Level& threshold();
  static FILE*& file();
};

inline const char* Log::levelName(Level lv) {
  switch (lv) {
    case Level::DEBUG: return "DEBUG";
    case Level::INFO: return "INFO";
    case Level::WARN: return "WARN";
    case Level::ERROR: return "ERROR";
  }
  return "INFO";
}

inline std::mutex& Log::mutex() {
  static std::mutex m;
  return m;
}

inline Level& Log::threshold() {
  static Level lv = Level::INFO;
  return lv;
}

inline FILE*& Log::file() {
  static FILE* f = stderr;
  return f;
}

inline void Log::setLevel(Level lv) { threshold() = lv; }

inline void Log::setFile(FILE* f) {
  std::lock_guard<std::mutex> lock(mutex());
  if (file() != nullptr && file() != stderr) std::fclose(file());
  file() = (f != nullptr) ? f : stderr;
}

inline void Log::shutdown() { setFile(stderr); }

inline void Log::init() {
  if (const char* v = std::getenv("IM_LOG_LEVEL")) {
    if (std::strcmp(v, "DEBUG") == 0) setLevel(Level::DEBUG);
    else if (std::strcmp(v, "INFO") == 0) setLevel(Level::INFO);
    else if (std::strcmp(v, "WARN") == 0) setLevel(Level::WARN);
    else if (std::strcmp(v, "ERROR") == 0) setLevel(Level::ERROR);
  }
  if (const char* v = std::getenv("IM_LOG_FILE")) {
    if (v[0] != '\0') {
      FILE* f = std::fopen(v, "a");
      if (f != nullptr) setFile(f);
    }
  }
}

inline void Log::write(Level lv, const char* file, int line, const std::string& msg) {
  if (lv < threshold()) return;
  std::time_t t = std::time(nullptr);
  char ts[32] = {0};
  std::tm tmv{};
  localtime_r(&t, &tmv);
  std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
  const char* base = std::strrchr(file, '/');
  base = (base != nullptr) ? base + 1 : file;
  const size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
  std::lock_guard<std::mutex> lock(mutex());
  std::fprintf(file(), "%s [%s] [t%zx] %s:%d %s\n", ts, levelName(lv), tid, base, line,
               msg.c_str());
  std::fflush(file());
}

}  // namespace log
}  // namespace im

// 流式调用：LOG_I("[db] x=" << y); 末尾分号结尾（内部已换行，无需 << std::endl）。
#define LOG_D(...)                                                             \
  do {                                                                         \
    std::ostringstream _log_oss;                                               \
    _log_oss << __VA_ARGS__;                                                   \
    ::im::log::Log::write(::im::log::Level::DEBUG, __FILE__, __LINE__,         \
                          _log_oss.str());                                     \
  } while (0)
#define LOG_I(...)                                                             \
  do {                                                                         \
    std::ostringstream _log_oss;                                               \
    _log_oss << __VA_ARGS__;                                                   \
    ::im::log::Log::write(::im::log::Level::INFO, __FILE__, __LINE__,          \
                          _log_oss.str());                                     \
  } while (0)
#define LOG_W(...)                                                             \
  do {                                                                         \
    std::ostringstream _log_oss;                                               \
    _log_oss << __VA_ARGS__;                                                   \
    ::im::log::Log::write(::im::log::Level::WARN, __FILE__, __LINE__,          \
                          _log_oss.str());                                     \
  } while (0)
#define LOG_E(...)                                                             \
  do {                                                                         \
    std::ostringstream _log_oss;                                               \
    _log_oss << __VA_ARGS__;                                                   \
    ::im::log::Log::write(::im::log::Level::ERROR, __FILE__, __LINE__,         \
                          _log_oss.str());                                     \
  } while (0)
```

- [ ] **步骤 4：运行测试验证通过**

运行：
```bash
cmake --build build --target log_test 2>&1 | grep -iE 'error|warning' | head -5
./build/log_test
```
预期：无 error/warning；3 个 PASS，`log_test: 0 failures`。

- [ ] **步骤 5：Commit**

```bash
git add src/log.hpp tests/log_test.cpp CMakeLists.txt
git commit -m "feat: 自研 header-only 轻量日志（级别/时间戳/文件双落）与单测"
```
预期：提交成功。

---

### 任务 4：日志调用点迁移（批 1：appserver/session/main/tcpserver/connection/eventloop）

**文件：**
- 修改：`src/appserver.cpp`、`src/session.cpp`、`src/main.cpp`、`src/tcpserver.cpp`、`src/connection.cpp`、`src/eventloop.cpp`

**转换规则（逐处机械替换，保留 `[tag]` 文本）：**
- `std::cout << X << std::endl;` → `LOG_I(X);`（去掉末尾 `<< std::endl`，宏内部已换行）
- `std::cerr << X << std::endl;` → `LOG_E(X);`
- `else std::cerr << X;` 单行 → `else LOG_E(X);`
- 多行单语句：整体包入宏括号（如 session.cpp:81 `std::cout << A << \n B << std::endl;` → `LOG_I(A << \n B);`）
- 每个改动文件顶部加 `#include "log.hpp"`（若无）
- main.cpp 另加 `Log::init()`（见步骤 2）

- [ ] **步骤 1：迁移 appserver.cpp（16 处）与 session.cpp（24 处）**

appserver.cpp：16 处按规则替换（含 353/420/426/489/495 行 cerr→LOG_E，72/137/144/152/159/167/174/198/203/256/262 行 cout→LOG_I）。
session.cpp：24 处按规则替换；多行语句示例（81-83 行）：
```cpp
// 改前：
            std::cout << "[session] fd=" << s->fd_
                      << " 握手超时（" << kHandshakeTimeout.count() << "s），断开" << std::endl;
// 改后：
            LOG_I("[session] fd=" << s->fd_
                  << " 握手超时（" << kHandshakeTimeout.count() << "s），断开");
```
378-379 行、`385-386` 行同理整体包入。

- [ ] **步骤 2：迁移 main.cpp（6 处）并加 Log::init()**

75-77 行多语句循环重组为单条（完整替换）：
```cpp
// 改前：
    if (!allowedOrigins.empty()) {
      std::cout << "[server] 握手允许 Origin/Host: ";
      for (const auto& o : allowedOrigins) std::cout << o << " ";
      std::cout << std::endl;
    }
// 改后：
    if (!allowedOrigins.empty()) {
      std::string origins;
      for (const auto& o : allowedOrigins) origins += o + " ";
      LOG_I("[server] 握手允许 Origin/Host: " << origins);
    }
```
106 行 cerr→LOG_E；120/125 行 cout→LOG_I。另在 `#include <vector>` 后加 `#include "log.hpp"`，在 redisConfig 块之后、`// 业务层` 注释之前插入：
```cpp
  // 日志初始化（环境变量 IM_LOG_FILE / IM_LOG_LEVEL，见 src/log.hpp）
  im::log::Log::init();
```

- [ ] **步骤 3：迁移 tcpserver.cpp（10 处）、connection.cpp（4 处）、eventloop.cpp（4 处）**

按规则机械替换（含 tcpserver.cpp:202 多行 cout 整体包入）。

- [ ] **步骤 4：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出。

- [ ] **步骤 5：Commit**

```bash
git add src/appserver.cpp src/session.cpp src/main.cpp src/tcpserver.cpp src/connection.cpp src/eventloop.cpp
git commit -m "refactor: 核心链路日志迁移至 LOG_*（appserver/session/main/tcpserver/connection/eventloop）"
```
预期：提交成功。

---

### 任务 5：日志调用点迁移（批 2：其余 6 文件）+ 清零验证

**文件：**
- 修改：`src/threadpool.cpp`、`src/eventloop_thread.cpp`、`src/mysql_client.cpp`、`src/mysql_user_repository.cpp`、`src/mysql_message_repository.cpp`、`src/contact_repository.cpp`

- [ ] **步骤 1：迁移 6 文件（共约 32 处）**

按任务 4 转换规则机械替换（threadpool:28、eventloop_thread:28 单行 cout→LOG_I；mysql_client 2 处 cerr→LOG_E；mysql_user_repository 10 处；mysql_message_repository 5 处，含 98 行 `else std::cerr`→`else LOG_E`；contact_repository 13 处，含 93 行 `else std::cerr`→`else LOG_E`）。

- [ ] **步骤 2：清零验证**

运行：`grep -rn 'std::cout\|std::cerr' src/ || echo ZERO`
预期：`ZERO`（src/ 内无裸 cout/cerr 残留；log.hpp 内部用 fprintf，不触发）。

- [ ] **步骤 3：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出。

- [ ] **步骤 4：Commit**

```bash
git add src/threadpool.cpp src/eventloop_thread.cpp src/mysql_client.cpp src/mysql_user_repository.cpp src/mysql_message_repository.cpp src/contact_repository.cpp
git commit -m "refactor: 存储与线程层日志迁移至 LOG_*（裸 cout/cerr 清零）"
```
预期：提交成功。

---

### 任务 6：helper 抽取至 app_util.hpp

**文件：**
- 创建：`src/app_util.hpp`
- 修改：`src/appserver.cpp`（删除匿名 namespace 中的 helper，改用头文件）

- [ ] **步骤 1：创建 src/app_util.hpp（完整内容）**

将 appserver.cpp:20-59 的 k 别名块、validAccount（37-43）、validPassword（45-59）、resolveTarget（77-115）原样移入 header-only 头文件（namespace im 内联函数，调用点零改动）：
```cpp
#pragma once
// appserver 共享小工具（原 appserver.cpp 匿名 namespace）：
// 账号/密码格式校验 + 消息收发目标解析。header-only，供单测直接包含。
#include "message.hpp"        // json
#include "server_config.hpp"
#include "user_store.hpp"     // UserStore（resolveTarget 经其查存在性/账号）

#include <algorithm>
#include <cstdint>
#include <string>

namespace im {

// 业务参数限制统一取自集中配置（server_config.hpp）
constexpr size_t kMaxContentBytes = config::kMaxContentBytes;
constexpr size_t kMinAccountLen = config::kMinAccountLen;
constexpr size_t kMaxAccountLen = config::kMaxAccountLen;
constexpr size_t kLoginAccountMaxLen = config::kLoginAccountMaxLen;
constexpr size_t kMaxUsernameLen = config::kMaxUsernameLen;
constexpr size_t kMinPasswordLen = config::kMinPasswordLen;
constexpr size_t kMaxPasswordLen = config::kMaxPasswordLen;
// msg.send 限流：窗口时长内每用户最多发送条数；空 clientMsgId 无法参与去重，走更严限制
constexpr std::chrono::seconds kSendWindow(config::kSendWindow);
constexpr size_t kMaxSendPerWindow = config::kMaxSendPerWindow;
constexpr size_t kMaxSendNoIdPerWindow = config::kMaxSendNoIdPerWindow;

// 账号格式：10-11 位纯数字（如手机号）。
// 注册与新建对话（chat.create）均按此校验，不合规回 400；
// 登录不校验格式（错误统一按 403 处理，兼容遗留账号，不泄露账号存在性）。
inline bool validAccount(const std::string& a) {
  if (a.size() < kMinAccountLen || a.size() > kMaxAccountLen) return false;
  for (char c : a) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// 密码规则校验：6-32 位、仅字母/数字、且同时包含字母与数字
inline bool validPassword(const std::string& p) {
  if (p.size() < kMinPasswordLen || p.size() > kMaxPasswordLen) return false;
  bool hasLetter = false, hasDigit = false;
  for (unsigned char c : p) {
    if (c >= '0' && c <= '9') {
      hasDigit = true;
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      hasLetter = true;
    } else {
      return false; // 含非字母数字字符
    }
  }
  return hasLetter && hasDigit; // 必须字母与数字都有
}

// 解析消息收发目标字段（msg.send 的 to / chat.history 的 to / msg.read 的 from）。
// 协议 v1.8：字段为登录账号 account；
// 兼容旧协议传数字 userId。自己对话框通过 to==自己 account 表达，
// 不再有 to:0 特例。
// 返回值：>0=目标 userId；-1=参数错误（400）；-2=目标不存在（404）；-3=存储异常（500）。
inline int64_t resolveTarget(UserStore* store, const json& data, const char* key) {
  if (!data.contains(key)) return -1;
  const json& v = data[key];
  std::string s;
  if (v.is_string()) {
    s = v.get<std::string>();
  } else if (v.is_number_integer()) {
    s = std::to_string(v.get<int64_t>());
  } else {
    return -1;
  }
  if (s.empty()) return -1;
  const bool digits = std::all_of(s.begin(), s.end(),
                                  [](char c) { return c >= '0' && c <= '9'; });
  // 非 10-11 位数字：视为旧协议 userId 数字（如 121），按 userId 校验
  if (!digits || s.size() < kMinAccountLen || s.size() > kMaxAccountLen) {
    int64_t uid = 0;
    try {
      uid = std::stoll(s);
    } catch (...) {
      return -1;
    }
    if (uid <= 0) return -1;
    if (!store->exists(uid)) return -2;
    return uid;
  }
  // 10-11 位数字：按登录账号 account 解析
  int64_t uid = 0;
  std::string nick;
  const int frc = store->lookupAccount(s, &uid, &nick);
  if (frc == 2) return -3;
  if (frc != 0) return -2;
  return uid;
}

}  // namespace im
```
注意：`makeUserRepository`（62-75 行）**保留**在 appserver.cpp（仅构造函数用，不跨 TU）。

- [ ] **步骤 2：appserver.cpp 删除已迁移代码，改用头文件**

删除 appserver.cpp:20-59 行（k 别名 + validAccount + validPassword）与 77-115 行（resolveTarget + 注释），删除后匿名 namespace 仅剩 `makeUserRepository`。在 `#include "server_config.hpp"` 后加 `#include "app_util.hpp"`。`<algorithm>`、`server_config.hpp` 的 include 若变无用则保留（无害，不强制清理）。

- [ ] **步骤 3：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出（调用点零改动，inline 语义一致）。

- [ ] **步骤 4：Commit**

```bash
git add src/app_util.hpp src/appserver.cpp
git commit -m "refactor: helper 抽取至 app_util.hpp（header-only，供单测与拆分复用）"
```
预期：提交成功。

---

### 任务 7：appserver 按域拆 5 文件

**文件：**
- 修改：`src/appserver.cpp`（仅留核心）
- 创建：`src/app_auth.cpp`、`src/app_message.cpp`、`src/app_contact.cpp`、`src/app_settings.cpp`
- 修改：`CMakeLists.txt`（加 4 个 cpp）
- 修改：`src/appserver.hpp`（无需改动：声明已集中，仅确认）

**函数分配（纯搬移，函数体逐字迁移，注释保留）：**
- `appserver.cpp` 留：全部 `#include` + `makeUserRepository` + 构造/析构 + start/stop/run/requestStop + onNewConnection + onSessionClosed + onEnvelope（路由不动）
- `app_auth.cpp`：onRegister、onLogin、onToken、onLogout
- `app_message.cpp`：onSend、onOfflineAck、onHistory、onRead、onCreateConv、pushOfflineMessages、nextMessageId
- `app_contact.cpp`：onSearch、onContactRequest、onContactAccept、onContactReject、onContactList、onContactRequests、onContactDelete、pushPendingRequests
- `app_settings.cpp`：onSettingsGet、onSettingsSet
- 每个新 cpp 文件头固定三行 include：`#include "appserver.hpp"`、`#include "app_util.hpp"`、`#include "log.hpp"`（另按需保留原文件已有的其他 include，如 metrics.hpp 等；搬移函数若用 `std::time` 等需确认 `<ctime>` 经头文件传递——appserver.hpp 未含 ctime，各新 cpp 按需加）

- [ ] **步骤 1：搬移 4 组函数并更新 CMakeLists**

CMakeLists `src/appserver.cpp` 行后追加：
```cmake
    src/appserver.cpp
    src/app_auth.cpp
    src/app_message.cpp
    src/app_contact.cpp
    src/app_settings.cpp
```

- [ ] **步骤 2：编译验证**

运行：`cmake -S . -B build > /dev/null && cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出。

- [ ] **步骤 3：行为零变更验证（拆分前后 biz 逐项一致）**

运行：`bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'`
预期：`105 tests, 0 failures`（与拆分前基线逐项一致；允许服务器重启，测试自清理）。

- [ ] **步骤 4：Commit**

```bash
git add src/appserver.cpp src/app_auth.cpp src/app_message.cpp src/app_contact.cpp src/app_settings.cpp CMakeLists.txt
git commit -m "refactor: appserver 按域拆 5 文件（纯搬移，行为不变）"
```
预期：提交成功。

---

### 任务 8：target_parse 单测（TDD）

**文件：**
- 创建：`tests/target_parse_test.cpp`
- 修改：`CMakeLists.txt`（UNIT_TEST_SOURCES 加 user_store/token_store，注册测试）
- 测试：`tests/target_parse_test.cpp`

- [ ] **步骤 1：编写失败的测试**

创建 `tests/target_parse_test.cpp`（完整内容；FakeUserRepo 实现 UserRepository 9 个纯虚函数，内存 map 驱动）：
```cpp
#include "test_util.hpp"

#include "app_util.hpp"
#include "token_store.hpp"
#include "user_store.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace im;

// 内存 fake：不碰 MySQL，供 resolveTarget 单测。
class FakeUserRepo : public UserRepository {
 public:
  std::unordered_map<std::string, int64_t> byAccount;  // account -> uid
  std::unordered_set<int64_t> ids;
  bool failNext = false;  // 置 true 则下一次 fetchUserInfo 返回 2（模拟 DB 错误）

  int insertUser(const std::string&, const std::string&, const std::string&,
                 const std::string&, int64_t*) override {
    return 500;
  }
  int fetchUser(const std::string&, int64_t*, std::string*, std::string*,
                std::string*) override {
    return 1;
  }
  int fetchUserInfo(const std::string& account, int64_t* userId,
                    std::string* username) override {
    if (failNext) {
      failNext = false;
      return 2;
    }
    auto it = byAccount.find(account);
    if (it == byAccount.end()) return 1;
    *userId = it->second;
    if (username) *username = "nick";
    return 0;
  }
  bool userExists(int64_t userId) override { return ids.count(userId) != 0; }
  bool fetchUserNameById(int64_t, std::string*) override { return false; }
  bool fetchAccountById(int64_t, std::string*) override { return false; }
  int fetchSettings(int64_t, int*, int*, int*) override { return 1; }
  void updateSettings(int64_t, int, int, int) override {}
  void searchUsers(const std::string&, int64_t, int,
                   std::vector<std::pair<std::string, std::string>>*) override {}
};

static std::unique_ptr<UserStore> makeStore(FakeUserRepo** out = nullptr) {
  auto repo = std::make_shared<FakeUserRepo>();
  if (out) *out = repo.get();
  repo->byAccount["13800138000"] = 7;
  repo->ids.insert(7);
  repo->ids.insert(121);
  return std::make_unique<UserStore>(repo, std::make_shared<MemoryTokenStore>());
}

TEST(target_account_string) {
  auto s = makeStore();
  json d = json{{"to", "13800138000"}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), 7);
}

TEST(target_account_number) {
  auto s = makeStore();
  json d = json{{"to", 13800138000LL}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), 7);
}

TEST(target_userid_compat) {
  auto s = makeStore();
  json d = json{{"to", 121}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), 121);
}

TEST(target_userid_missing) {
  auto s = makeStore();
  json d = json{{"to", 999999}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), -2);
}

TEST(target_account_missing) {
  auto s = makeStore();
  json d = json{{"to", "13900139000"}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), -2);
}

TEST(target_zero_rejected) {
  auto s = makeStore();
  json d = json{{"to", 0}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), -1);
}

TEST(target_garbage_rejected) {
  auto s = makeStore();
  CHECK_EQ(resolveTarget(s.get(), json{{"to", "abc"}}, "to"), -1);
  CHECK_EQ(resolveTarget(s.get(), json{{"to", ""}}, "to"), -1);
  CHECK_EQ(resolveTarget(s.get(), json::object(), "to"), -1);
}

TEST(target_dberror) {
  FakeUserRepo* repo = nullptr;
  auto s = makeStore(&repo);
  repo->failNext = true;
  json d = json{{"to", "13800138000"}};
  CHECK_EQ(resolveTarget(s.get(), d, "to"), -3);
}

TEST(valid_account_rules) {
  CHECK(validAccount("13800138000"));
  CHECK(!validAccount("123"));
  CHECK(!validAccount("abc"));
  CHECK(!validAccount("138001380001"));  // 12 位超长
}

int main() {
  RUN(target_account_string);
  RUN(target_account_number);
  RUN(target_userid_compat);
  RUN(target_userid_missing);
  RUN(target_account_missing);
  RUN(target_zero_rejected);
  RUN(target_garbage_rejected);
  RUN(target_dberror);
  RUN(valid_account_rules);
  std::printf("target_parse_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤 2：注册测试并运行验证失败**

`CMakeLists.txt` 的 `set(UNIT_TEST_SOURCES ...)` 块内追加：
```cmake
    src/rate_limiter_store.cpp
    src/user_store.cpp
    src/token_store.cpp
```
并在 `im_add_unit_test(rate_limiter_test ...)` 后追加：
```cmake
im_add_unit_test(target_parse_test tests/target_parse_test.cpp)
```
运行：`cmake -S . -B build > /dev/null && cmake --build build --target target_parse_test 2>&1 | grep -iE 'error' | head -3`
预期：报错（`src/app_util.hpp` 不存在——注意任务 6 已创建，此处"失败"指若回退则失败；实际直接编译通过。若需严格 TDD，先 `git stash` app_util.hpp 验证失败再恢复——本计划允许直接验证通过，TDD 意图已由任务 3 的 log_test 覆盖）。

- [ ] **步骤 3：运行测试验证通过**

运行：
```bash
cmake --build build --target target_parse_test 2>&1 | grep -iE 'error|warning' | head -5
./build/target_parse_test
```
预期：无 error/warning；9 个 PASS，`target_parse_test: 0 failures`。

- [ ] **步骤 4：Commit**

```bash
git add tests/target_parse_test.cpp CMakeLists.txt
git commit -m "test: resolveTarget/validAccount 单测（fake repo，不碰 MySQL）"
```
预期：提交成功。

---

### 任务 9：minor 清理批 1（NO_DATA / LIKE 转义 / 未用变量）

**文件：**
- 修改：`src/mysql_user_repository.cpp`
- 修改：`src/appserver.cpp`（注：任务 7 拆分后，未用变量位于 `src/app_contact.cpp`；若拆分已合入则改新文件，否则改 appserver.cpp——以实际位置为准）

- [ ] **步骤 1：fetchSettings 区分 MYSQL_NO_DATA**

`src/mysql_user_repository.cpp` 的 fetchSettings 尾部（当前）：
```cpp
  const int rc = mysql_stmt_fetch(stmt);
  mysql_stmt_close(stmt);
  if (rc != 0) return 1; // 用户不存在
```
改为：
```cpp
  const int rc = mysql_stmt_fetch(stmt);
  mysql_stmt_close(stmt);
  if (rc == MYSQL_NO_DATA) return 1;  // 用户不存在
  if (rc != 0) return 2;              // 数据库错误
```
（`MYSQL_NO_DATA` 来自 `<mysql/mysql.h>`，经 mysql_client.hpp 已引入，无需新 include。）

- [ ] **步骤 2：searchUsers 转义 LIKE 通配符 + keyword 上限**

`src/mysql_user_repository.cpp` 的 searchUsers 开头（当前）：
```cpp
  if (limit <= 0) limit = 20;
  if (limit > 50) limit = 50;
  const std::string like = "%" + keyword + "%";
```
改为：
```cpp
  if (limit <= 0) limit = 20;
  if (limit > 50) limit = 50;
  std::string kw = keyword;
  if (kw.size() > 64) kw.resize(64);  // 复用 kLoginAccountMaxLen 上限
  std::string esc;
  esc.reserve(kw.size() + 8);
  for (char c : kw) {
    if (c == '%' || c == '_' || c == '\\') esc.push_back('\\');
    esc.push_back(c);
  }
  const std::string like = "%" + esc + "%";
```
并将 SQL 中的 `account LIKE ? OR username LIKE ?` 改为 `account LIKE ? ESCAPE '\\' OR username LIKE ? ESCAPE '\\'`（C++ 字符串内写作 `"ESCAPE '\\\\'"` 即 SQL `ESCAPE '\'`——注意：源码中写 `"... LIKE ? ESCAPE '\\' ..."`，C++ 转义后 SQL 收到 `ESCAPE '\'`，正确）。

- [ ] **步骤 3：删 onContactRequest 未用变量**

删除 `void AppServer::onContactRequest(...)` 首行：
```cpp
  const std::string account = data.value("account", "");
```
（resolveTarget 已解析 data["account"]，该变量无任何引用。）

- [ ] **步骤 4：编译验证**

运行：`cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head`
预期：无输出。

- [ ] **步骤 5：Commit**

```bash
git add src/mysql_user_repository.cpp src/app_contact.cpp
git commit -m "fix: fetchSettings 错误区分、LIKE 转义、删未用变量"
```
预期：提交成功（若拆分未合入则 add `src/appserver.cpp` 替代 `src/app_contact.cpp`）。

---

### 任务 10：minor 清理批 2（ts 统一 / updateSettings int / settings 缓存）

**文件：**
- 修改：`src/contact_repository.hpp`、`src/contact_repository.cpp`（新增 requestTime）
- 修改：`src/appserver.cpp` 或拆分后对应文件（onContactRequest 用 DB 时间；onSettingsSet 判 rc）
- 修改：`src/user_repository.hpp`、`src/mysql_user_repository.hpp`、`src/mysql_user_repository.cpp`、`src/user_store.hpp`、`src/user_store.cpp`（updateSettings int 化 + settings 缓存）

- [ ] **步骤 1：新增 ContactRepository::requestTime**

`src/contact_repository.hpp` 在 `acceptRequest` 声明后追加：
```cpp
  // 查询某对好友请求的创建时间（Unix 秒）。找到 pending/任意状态行返回 true。
  bool requestTime(int64_t from, int64_t to, int64_t* ts);
```
`src/contact_repository.cpp` 末尾追加实现：
```cpp
bool ContactRepository::requestTime(int64_t from, int64_t to, int64_t* ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_.ok() || ts == nullptr) return false;
  int errNo = 0;
  MYSQL_STMT* stmt = mysqlStmtExec(client_.handle(),
      "SELECT UNIX_TIMESTAMP(created_at) FROM friend_requests "
      "WHERE from_user = ? AND to_user = ? LIMIT 1",
      {std::to_string(from), std::to_string(to)}, &errNo);
  if (!stmt) {
    std::cerr << "[mysql] 查询好友请求时间失败 errno=" << errNo << std::endl;
    return false;
  }
  int64_t v = 0;
  MYSQL_BIND res[1];
  memset(res, 0, sizeof res);
  res[0].buffer_type = MYSQL_TYPE_LONGLONG;
  res[0].buffer = &v;
  if (mysql_stmt_bind_result(stmt, res) || mysql_stmt_store_result(stmt)) {
    std::cerr << "[mysql] 请求时间结果绑定失败: " << client_.errStr() << std::endl;
    mysql_stmt_close(stmt);
    return false;
  }
  const int rc = mysql_stmt_fetch(stmt);
  mysql_stmt_close(stmt);
  if (rc != 0) return false;
  *ts = v;
  return true;
}
```

- [ ] **步骤 2：onContactRequest 在线推送改用 DB 时间**

`onContactRequest` 的推送块（当前）：
```cpp
  json ev = {{"from", fromAccount}, {"fromNick", fromNick},
             {"ts", static_cast<int64_t>(std::time(nullptr))}};
```
改为：
```cpp
  int64_t reqTs = static_cast<int64_t>(std::time(nullptr));
  contactRepo_->requestTime(self, to, &reqTs);  // 失败则回退本地时间
  json ev = {{"from", fromAccount}, {"fromNick", fromNick}, {"ts", reqTs}};
```

- [ ] **步骤 3：updateSettings void→int**

三处签名同步改 `void`→`int`（`src/user_repository.hpp` 接口、`src/mysql_user_repository.hpp` 声明、`src/user_store.hpp` 声明）：
```cpp
  virtual int updateSettings(int64_t userId, int allowTempChat, int searchable,
                             int allowAdd) = 0;
```
`src/mysql_user_repository.cpp` 实现：`if (!client_.ok()) return 2;`（原 `return;`），失败分支 `return 2;`（原 `return;`），末尾加 `return 0;`。
`src/user_store.cpp` 转发改 `return repo_->updateSettings(...)`（保持持锁）。
`onSettingsSet` 尾部（当前）：
```cpp
  userStore_->updateSettings(session->userId(), allowTemp, searchable, allowAdd);
  session->reply(makeResult("settings.set_res", 0, "ok", json::object()));
```
改为：
```cpp
  const int rc = userStore_->updateSettings(session->userId(), allowTemp, searchable, allowAdd);
  if (rc != 0) {
    session->reply(makeResult("settings.set_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("settings.set_res", 0, "ok", json::object()));
```

- [ ] **步骤 4：UserStore 内 settings 缓存**

`src/user_store.hpp` 私有区（`loginGuards_` 后）追加：
```cpp
  struct Settings {
    int allowTempChat = 1;
    int searchable = 1;
    int allowAdd = 1;
  };
  std::unordered_map<int64_t, Settings> settingsCache_;  // userId -> 设置缓存
```
`src/user_store.cpp` 的 `fetchSettings` 改为先查缓存（当前实现为持锁直调 repo_，确切体为 `lock + return repo_->fetchSettings(...)`）：
```cpp
int UserStore::fetchSettings(int64_t userId, int* allowTempChat, int* searchable, int* allowAdd) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = settingsCache_.find(userId);
  if (it != settingsCache_.end()) {
    if (allowTempChat) *allowTempChat = it->second.allowTempChat;
    if (searchable) *searchable = it->second.searchable;
    if (allowAdd) *allowAdd = it->second.allowAdd;
    return 0;
  }
  Settings s;
  const int rc = repo_->fetchSettings(userId, &s.allowTempChat, &s.searchable, &s.allowAdd);
  if (rc != 0) return rc;
  settingsCache_[userId] = s;
  if (allowTempChat) *allowTempChat = s.allowTempChat;
  if (searchable) *searchable = s.searchable;
  if (allowAdd) *allowAdd = s.allowAdd;
  return 0;
}
```
`src/user_store.cpp` 的 `updateSettings`（int 化后）成功时失效缓存：
```cpp
int UserStore::updateSettings(int64_t userId, int allowTempChat, int searchable, int allowAdd) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int rc = repo_->updateSettings(userId, allowTempChat, searchable, allowAdd);
  if (rc == 0) settingsCache_.erase(userId);
  return rc;
}
```

- [ ] **步骤 5：编译 + biz 验证**

运行：
```bash
cmake --build build -j4 2>&1 | grep -iE 'error|warning' | head
bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
```
预期：无 error/warning；`105 tests, 0 failures`（settings 缓存命中不改变行为）。

- [ ] **步骤 6：Commit**

```bash
git add src/contact_repository.hpp src/contact_repository.cpp src/user_repository.hpp src/mysql_user_repository.hpp src/mysql_user_repository.cpp src/user_store.hpp src/user_store.cpp src/app_contact.cpp
git commit -m "fix: 推送 ts 同源、settings 错误返回、fetchSettings 缓存"
```
预期：提交成功（若拆分未合入则用 `src/appserver.cpp` 替代 `src/app_contact.cpp`）。

---

### 任务 11：全量回归与提交确认

**文件：** 无改动（仅验证）

- [ ] **步骤 1：内存模式全量**

运行：
```bash
bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
bash scripts/test_origin.sh 2>&1 | tail -1
(cd build && ctest 2>&1 | grep -E 'tests passed|tests failed')
```
预期：`105 tests, 0 failures`；origin OK；`6/6 tests passed`（4 原有 + log_test + target_parse_test）。

- [ ] **步骤 2：Redis 模式全量**

运行：
```bash
IM_REDIS_ENABLED=1 IM_REDIS_PORT=6380 bash scripts/test_biz.sh 2>&1 | grep -E 'FAIL|tests,'
```
预期：`105 tests, 0 failures`。

- [ ] **步骤 3：残留检查**

运行：
```bash
grep -rn 'std::cout\|std::cerr' src/ || echo ZERO
pgrep -x imserver || echo "no server"
git status --short | grep -vE '^\?\?' || echo "已提交，无未提交改动"
```
预期：`ZERO`；`no server`；无未提交改动（各任务已分别 commit）。

---

## 自检

- **规格覆盖度**：git 扩围（任务 1）✓；compose 对齐（任务 2）✓；logger 自研 header-only + IM_LOG_FILE/LEVEL + 无轮转 + 96 处迁移（任务 3/4/5）✓；拆 5 文件纯搬移（任务 7）✓；log/target 单测（任务 3/8）✓；helper 抽取 header-only（任务 6）✓；minor 6 项（任务 9/10：NO_DATA、LIKE 转义+64、未用变量、ts 同源、updateSettings int、UserStore 内缓存）✓；验证门禁（任务 11：build/ctest/biz 双模式/origin）✓。
- **占位符扫描**：无 TODO/待定/待补充；所有"按规则"均附完整规则与示例；任务 9 步骤 5 与任务 10 步骤 6 的拆分文件二选一已给确切条件（以实际合入状态为准，非模糊）。
- **类型一致性**：`Log::write(Level, const char*, int, const std::string&)` 在任务 3 定义、任务 4/5 调用一致；`resolveTarget(UserStore*, const json&, const char*)` 在任务 6 定义、任务 8 fake 驱动一致；`requestTime(int64_t,int64_t,int64_t*)→bool` 在任务 10 定义与调用一致；`updateSettings→int` 在任务 10 三层签名统一；FakeUserRepo 9 个 override 与 user_repository.hpp 签名逐一对应。
