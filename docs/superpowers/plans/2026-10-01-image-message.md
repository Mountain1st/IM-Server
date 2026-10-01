# 图片/表情消息 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 落地图片消息（text/image 双 msg_type）+ P0 emoji 零改动验证，保持 210 用例双模式全绿。

**架构：** WS 信封解析 msg_type 分流校验 → base64 廉价检查 → 按用户图片限流 + 并发闸(2) → 解码+magic+维度 → HMAC 对象键落盘 uploads/ → DB 存 `/uploads/<file>` + msg_type 列 → 单聊/群聊/离线/历史全透传；nginx alias 只读服务 + 双向命名卷。

**技术栈：** C++17 / MySQL 8 / Redis 8 / Nginx 1.27 / CMake+CTest / Python biz_test

---

## 文件结构

- 修改：`src/server_config.hpp`——新增 `kMaxImageB64Bytes=1.5MB`、`kImagePerWindow=10`、`kImageWindow=60s`、`kMaxConcurrentImageDecode=2`、`kMaxImageDimensionPx=4096`
- 修改：`src/message_repository.hpp`——`StoredMessage` 加 `msgType`（默认 "text"），`saveMessage` 加 `msgType` 参数（默认 "text" 保持兼容）
- 修改：`src/mysql_message_repository.{hpp,cpp}`——`kMsgCols 9→10`，`rowToMessage`、`saveMessage`、三条 SELECT（fetchOffline/fetchHistory/fetchGroupMessages）加 `msg_type`
- 新增：`db/migrate_msg_type.sql`——`ALTER TABLE messages ADD COLUMN msg_type VARCHAR(16) NOT NULL DEFAULT 'text'`
- 新增：`src/media_store.{hpp,cpp}`——base64 字符集检查、解码、magic+维度解析（PNG/JPEG/GIF/WebP，拒 SVG）、HMAC-SHA256 对象键、tmp+fsync+rename 原子落盘
- 修改：`src/app_message.cpp`——msg_type 解析→白名单→廉价检查→图片限流/并发闸→解码落盘→存 URL+msg_type；ack 不带 msg_type
- 修改：`src/app_group.cpp`——`onGroupSend` 同分流 + 群历史/补发序列化带 msg_type
- 修改：`src/appserver.{hpp,cpp}`——uploads 目录自建+可写校验、清理线程加 media GC（mtime 7 天，只删命名模式常规文件，跳过 symlink/*.tmp）
- 修改：`nginx/im.conf`——加 `location /uploads/`（alias 只读，`max-age=86400`，仅服务命名模式文件）
- 修改：`docker-compose.yml`——新增 `uploads_data` 卷，imserver 读写挂 `/app/uploads`，nginx 只读挂；Dockerfile 加 `mkdir -p /app/uploads && chown im:im`
- 测试：`tests/test_media_store.cpp`（新增，ctest）、`scripts/biz_test.py`（加 image 负例矩阵用例）
- 文档：`docs/protocol.md`、`README.md`（msg_type 语义、过期即 404、max-age 1 天）

---

### 任务 1：P0 emoji 链路验证（零改动预期）

**文件：**
- 测试：`scripts/biz_test.py`（临时本地脚本验证，不入库）

- [ ] **步骤 1：编写失败的测试（手动验证脚本）**

```python
# 本地验证：昵称/群公告/clientMsgId/消息 content 带 emoji 全链路
cases = ["hi 👋", "昵称🎉", "公告📢", "clientMsgId-🙂-001"]
for c in cases:
    r = send_and_recv(c)
    assert r["content"] == c, r
```

- [ ] **步骤 2：运行测试验证现状**

运行：`IM_REDIS_ENABLED=1 IM_REDIS_PORT=6379 bash scripts/test_biz.sh 2>&1 | tail -5`
预期：PASS（210/210），emoji 字段入库为 utf8mb4 无乱码

- [ ] **步骤 3：无需实现代码（若失败则按失败项修 utf8 链）**
- [ ] **步骤 4：记录结论到本计划执行日志**
- [ ] **步骤 5：Commit（仅文档结论，若有）**

```bash
git add docs/superpowers/plans/2026-10-01-image-message.md
git commit -m "docs: P0 emoji 链路验证结论"
```

---

### 任务 2：存储层 msg_type（最大改动面先行）

**文件：**
- 修改：`src/message_repository.hpp:10-20`
- 修改：`src/mysql_message_repository.hpp`、`src/mysql_message_repository.cpp:28-76`
- 新增：`db/migrate_msg_type.sql`
- 测试：`tests/test_mysql_repository.cpp`（按现有文件名，若无则用 ctest 现有单测+本地 SQL 验证）

- [ ] **步骤 1：编写失败的测试**

```cpp
// 保存带 msg_type=image 的消息，fetchHistory 应原样带回
repo.saveMessage("mid-img-1", 1, 2, "/uploads/abc.png", now, "cid-1", "single", 1, 0, "image");
auto h = repo.fetchHistory(1, 2, 10);
EXPECT_EQ(h[0].msgType, "image");
EXPECT_EQ(h[0].content, "/uploads/abc.png");
```

- [ ] **步骤 2：运行测试验证失败**

运行：`cmake --build build -j4 && ctest --test-dir build 2>&1 | tail -10`
预期：FAIL（无 msgType 字段/列）

- [ ] **步骤 3：编写最少实现代码**

```cpp
// message_repository.hpp
std::string msgType = "text";
// saveMessage 签名末尾加：const std::string& msgType = "text"
// mysql：kMsgCols 9→10，三条 SELECT 末尾加 msg_type，rowToMessage 取 str(9) 为空则 "text"
// saveMessage SQL 加 msg_type 列与占位符
// db/migrate_msg_type.sql：
ALTER TABLE messages ADD COLUMN IF NOT EXISTS msg_type VARCHAR(16) NOT NULL DEFAULT 'text';
```

- [ ] **步骤 4：运行测试验证通过**

运行：`cmake --build build -j4 && ctest --test-dir build 2>&1 | tail -5`
预期：PASS，7/7

- [ ] **步骤 5：Commit**

```bash
git add src/message_repository.hpp src/mysql_message_repository.* db/migrate_msg_type.sql
git commit -m "feat: 存储层 msg_type 透传（默认text兼容）"
```

---

### 任务 3：MediaStore 单元（解码+命名+落盘）

**文件：**
- 创建：`src/media_store.hpp`、`src/media_store.cpp`
- 修改：`CMakeLists.txt`（加源文件与测试目标）
- 测试：`tests/test_media_store.cpp`

- [ ] **步骤 1：编写失败的测试**

```cpp
// 1x1 PNG base64 应解码成功并返回 /uploads/<64hex>.png；SVG 应拒绝；超大维度拒绝
auto ok = MediaStore::decodeAndStore(png1x1_b64, "/tmp/uploads-test");
EXPECT_TRUE(ok.has_value());
EXPECT_EQ(MediaStore::validate("not-base64!!!"), false);
EXPECT_EQ(MediaStore::detectKind(svg_bytes), MediaKind::kReject);
```

- [ ] **步骤 2：运行测试验证失败**

运行：`cmake --build build -j4 && ctest --test-dir build -R media 2>&1 | tail -10`
预期：FAIL（文件不存在）

- [ ] **步骤 3：编写最少实现代码**

```cpp
// media_store.hpp
enum class MediaKind { kPng, kJpeg, kGif, kWebp, kReject };
class MediaStore {
 public:
  static bool isValidBase64(std::string_view s);
  static std::optional<std::vector<uint8_t>> b64Decode(std::string_view s);
  static MediaKind detectKind(const std::vector<uint8_t>& bytes, int* w = nullptr, int* h = nullptr);
  // HMAC-SHA256(内容, 服务端密钥) 全长 hex + 扩展名；tmp+fsync+rename
  static std::optional<std::string> decodeAndStore(std::string_view b64, const std::string& dir);
};
```

实现要点：PNG 读 IHDR 宽高、JPEG 扫 SOF0-SOF3、GIF 逻辑屏幕、WebP 查 VP8/VP8L 头取维度；维度任一边 >4096 拒绝；SVG（含 `<svg`）直接拒绝；日志只记大小/mime，不打字节。

- [ ] **步骤 4：运行测试验证通过**

运行：`cmake --build build -j4 && ctest --test-dir build -R media 2>&1 | tail -5`
预期：PASS

- [ ] **步骤 5：Commit**

```bash
git add src/media_store.* tests/test_media_store.cpp CMakeLists.txt
git commit -m "feat: MediaStore 解码+维度+ HMAC对象键落盘"
```

---

### 任务 4：单聊图片路径（校验顺序重排）

**文件：**
- 修改：`src/server_config.hpp:42-43`（新增图片常量）
- 修改：`src/app_message.cpp:104-160`

- [ ] **步骤 1：编写失败的测试（biz 层）**

```python
# msg_type=image + 1x1 PNG b64 应 ack 0 且 content 为 /uploads/*.png；
# 1.51MB 信封应 413；非法 b64 应 400；超 10 张/分钟应 429
r = send_msg(msg_type="image", content=PNG_1X1_B64)
assert r["code"] == 0 and r["content"].startswith("/uploads/")
```

- [ ] **步骤 2：运行测试验证失败**

运行：`bash scripts/test_biz.sh 2>&1 | tail -5`
预期：FAIL（当前 64KB 先拒杀死图片，返回 413）

- [ ] **步骤 3：编写最少实现代码**

```cpp
// onSend 重排（伪代码，落到 app_message.cpp）：
std::string msgType = data.value("msgType", "text");
if (msgType != "text" && msgType != "image") { ack(400, "参数错误"); return; }
if (msgType == "text") { /* 原 64KB 路径不变 */ }
else {
  if (content.empty() || content.size() > kMaxImageB64Bytes) { ack(413, "图片过大"); return; }
  if (!MediaStore::isValidBase64(content)) { ack(400, "参数错误"); return; }
  if (!imgLimiter_->allow(uid, kImagePerWindow, kImageWindow)) { ack(429, "图片发送过于频繁"); return; }
  // 并发闸 semaphore(2) → MediaStore::decodeAndStore → 失败 ack(400)
  // content 替换为 "/uploads/<file>" 后走原幂等/落库/投递路径，saveMessage(..., "image")
}
```

- [ ] **步骤 4：运行测试验证通过**

运行：`bash scripts/test_biz.sh 2>&1 | tail -3 && ctest --test-dir build 2>&1 | tail -3`
预期：内存模式 210+新增全绿，ctest 全绿

- [ ] **步骤 5：Commit**

```bash
git add src/app_message.cpp src/server_config.hpp scripts/biz_test.py
git commit -m "feat: 单聊图片路径（msg_type分流+限流+并发闸）"
```

---

### 任务 5：群聊/离线/历史透传 + 清理 GC

**文件：**
- 修改：`src/app_group.cpp`（onGroupSend 同分流）、群历史/补发序列化
- 修改：`src/appserver.cpp`（uploads 自建+可写校验、purge 轮内加 media GC last-run 判定）
- 测试：`scripts/biz_test.py`

- [ ] **步骤 1：编写失败的测试**

```python
# 群发 image 应全员收到 /uploads/ URL 且 history 带 msg_type=image；
# mtime 8 天文件被 GC 后 URL 访问 404 占位
```

- [ ] **步骤 2：运行测试验证失败**

运行：`bash scripts/test_biz.sh 2>&1 | tail -5`
预期：FAIL（群路径无分流/无 msg_type）

- [ ] **步骤 3：编写最少实现代码**

```cpp
// 群路径复用任务 4 分流函数；序列化 msg.new/msg.history 群补发加 "msgType"
// GC：purge 轮内 if (now - lastMediaGc > 24h) { scan uploads/; mtime>7天且匹配 ^[0-9a-f]{64}\.(png|jpg|gif|webp)$ 的常规文件才删 }
```

- [ ] **步骤 4：运行测试验证通过**

运行：`IM_REDIS_ENABLED=1 IM_REDIS_PORT=6379 bash scripts/test_biz.sh 2>&1 | tail -3`
预期：Redis 模式全绿

- [ ] **步骤 5：Commit**

```bash
git add src/app_group.cpp src/appserver.cpp scripts/biz_test.py
git commit -m "feat: 群聊图片透传+uploads GC"
```

---

### 任务 6：运维面（nginx/compose/Dockerfile）+ 文档回归

**文件：**
- 修改：`nginx/im.conf`、`docker-compose.yml`、`Dockerfile`、`docs/protocol.md`、`README.md`

- [ ] **步骤 1：编写验证（部署检查）**

```bash
docker compose config >/dev/null && echo compose-ok
docker compose up -d --build imserver nginx && sleep 3
curl -fsS http://127.0.0.1:9191/health && echo health-ok
```

- [ ] **步骤 2：执行并确认失败点（nginx 无卷时 /uploads/ 404）**
- [ ] **步骤 3：最少改动**

```nginx
location /uploads/ {
    alias /var/lib/imserver/uploads/;
    expires 1d;
    add_header Cache-Control "public, max-age=86400";
}
```

```yaml
volumes:
  uploads_data:
services:
  imserver:
    volumes: ["uploads_data:/app/uploads"]
  nginx:
    volumes: ["./nginx/im.conf:/etc/nginx/conf.d/im.conf:ro", "uploads_data:/var/lib/imserver/uploads:ro"]
```

Dockerfile 加：`RUN mkdir -p /app/uploads && chown im:im /app/uploads`（USER im 之前）

- [ ] **步骤 4：验证通过（health + 静态文件 + 双模式 biz 全绿 + 负例矩阵第 9 项）**
- [ ] **步骤 5：Commit**

```bash
git add nginx/im.conf docker-compose.yml Dockerfile docs/protocol.md README.md
git commit -m "feat: uploads 静态服务+共享卷+文档"
```

---

## 自检

- [x] 规格覆盖：共享卷/校验顺序/存储全量/HMAC 键/限流按用户/并发闸 2/离线双 7 天/ack 无 msg_type/max-age 1 天/过期即 404/群三处/清理 last-run/禁日志打字节/备份告警/负例矩阵——均有对应任务
- [x] 无占位符：每步含代码/命令/预期
- [x] 类型一致：msgType 字符串 "text"/"image"，DB `msg_type VARCHAR(16)`，URL `/uploads/<hex>.<ext>`
