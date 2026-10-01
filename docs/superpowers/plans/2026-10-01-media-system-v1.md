# 媒体系统 V1 实现计划（对象存储 + 预签名直传，仅图片）

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 把图片从「base64 塞 WS + 本地盘」升级为「MinIO 对象存储 + S3 SigV4 预签名直传 + media 表元数据」，服务端只做签名与元数据。

**架构：** 客户端经 WS `media.presign` 拿签名 PUT URL 直传 MinIO → `media.commit`（服务端 HEAD 校验）→ `msg.send(msgType=image, mediaId)` 只存 media_id；查看时 `media.url` 鉴权后签发短期 GET URL。签名代码单一套（方法参数化预签名），服务端 HEAD/DELETE 也用预签名 URL + libcurl。

**技术栈：** C++17 / 手写 SHA256+HMAC 复用（不引 OpenSSL）/ S3 SigV4 预签名 / libcurl（builder dev 头）/ MinIO / MySQL 8 / WebSocket JSON 信封 / CTest。

**规格：** `docs/superpowers/specs/2026-10-01-media-system-design.md`（本计划实现其 V1 期）。

---

## 关键既有事实（已核实，供实现者对齐）

- `src/util.cpp`：手写 `sha256Raw`（32B 二进制）、`sha256Hex`、`hmacSha256Raw`（任意二进制 key/data，RFC 4231 向量实测通过）；**未链接 OpenSSL**；`util.cpp:3` 注释错误声称 OpenSSL（本计划修）。
- `tests/` 现有 10 个单测，**无任何 crypto 测试**。
- 协议走 WS JSON 信封，路由表 `AppServer::routes_`（`appserver.cpp:buildRoutes`），Handler 签名 `void(AppServer::*)(const shared_ptr<Session>&, const json&)`。
- 消息仓库：`MessageRepository`（`message_repository.hpp`）/ `MySqlMessageRepository`（`kMsgCols=10`，三条 SELECT + save + rowToMessage）。
- 现图片路径：`app_message.cpp` onSend 的 `msgType=image` 分支（base64→`MediaStore::decodeAndStore`→本地盘）；群路径 `app_group.cpp:onGroupSend`。**V1 要替换为 mediaId**。
- 限流器工厂 `makeRateLimiter(c, label, keyPrefix)`（`appserver.cpp:106`），Redis 键前缀隔离。
- token 校验：`UserStore::verifyToken`（`user_store.hpp:60`）；WS 层登录态在 `AppServer::onEnvelope`。
- CMake 单测清单 `CMakeLists.txt:104-132`（`UNIT_TEST_SOURCES` + `im_add_unit_test`）。

## 文件结构

- 修改：`src/util.cpp:3`——修正 OpenSSL 错误注释。
- 创建：`tests/crypto_test.cpp`——SHA256/HMAC 确定性向量单测。
- 创建：`src/sigv4.hpp` / `src/sigv4.cpp`——纯函数 S3 SigV4 预签名（可单测，不依赖 libcurl）。
- 创建：`tests/sigv4_test.cpp`——SigV4 确定性向量单测。
- 创建：`src/object_store.hpp`——`ObjectStore` 接口 + `PresignedUrl`/`ObjectInfo`。
- 创建：`src/minio_object_store.{hpp,cpp}`——MinIO 实现（预签名 + libcurl HEAD/DELETE）。
- 创建：`tests/minio_object_store_test.cpp`——纯逻辑单测（URL 组装/编码），真机集成另见脚本。
- 创建：`src/media_repository.hpp` / `src/mysql_media_repository.{hpp,cpp}`——media 表存取。
- 创建：`db/migrate_media.sql`——media 表 + messages.media_id（幂等）。
- 修改：`db/schema.sql`——新库同步。
- 修改：`src/message_repository.hpp` + `src/mysql_message_repository.{hpp,cpp}`——`StoredMessage.mediaId`、save 加 mediaId、三条 SELECT + rowToMessage。
- 创建：`src/media_service.{hpp,cpp}`——presign/commit/url 业务编排 + 权限校验。
- 修改：`src/appserver.{hpp,cpp}`——构造媒体组件、注册 3 条路由、purge 接 GC。
- 修改：`src/app_message.cpp` / `src/app_group.cpp`——`msgType=image` 改存 mediaId；下发改带 mediaId。
- 修改：`src/server_config.hpp`——媒体常量。
- 修改：`CMakeLists.txt`——源文件 + 单测 + libcurl。
- 修改：`Dockerfile` / `docker-compose.yml` / `nginx/im.conf`——MinIO 服务、libcurl dev 头、下线 `/uploads/` 静态。
- 修改：`scripts/biz_test.py`——图片用例改走 presign/commit/url；负例矩阵。
- 文档：`docs/protocol.md`、`README.md`。

---

### 任务 1：修正 util.cpp 注释 + 补 SHA256/HMAC 向量单测

**文件：**
- 修改：`src/util.cpp:1-7`
- 创建：`tests/crypto_test.cpp`
- 修改：`CMakeLists.txt`（加 `crypto_test`）

- [ ] **步骤 1：编写失败的测试**

```cpp
// tests/crypto_test.cpp
#include "test_util.hpp"
#include "util.hpp"
#include <string>
using namespace im;

TEST(sha256_fips_vectors) {
  CHECK_EQ(sha256Hex(""),
      std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha256Hex("abc"),
      std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(hmac_sha256_rfc4231) {
  // TC1: key=20B 0x0b, data="Hi There"
  std::string k1(20, (char)0x0b);
  CHECK_EQ(hmacSha256Raw(k1, "Hi There"),
      std::string("\xb0\x34\x4c\x61\xd8\xdb\x38\x53\x5c\xa8\xaf\xce\xaf\x0b\xf1\x2b"
                  "\x88\x1d\xc2\x00\xc9\x83\x3d\xa7\x26\xe9\x37\x6c\x2e\x32\xcf\xf7", 32));
  // 注意：第 19 字节为 \x00，必须显式给长度 32，否则 std::string(const char*) 截断到 19
  // TC3: key=20B 0xaa, data=50B 0xdd
  std::string k3(20, (char)0xaa), d3(50, (char)0xdd);
  CHECK_EQ(hmacSha256Raw(k3, d3),
      std::string("\x77\x3e\xa9\x1e\x36\x80\x0e\x46\x85\x4d\xb8\xeb\xd0\x91\x81\xa7"
                  "\x29\x59\x09\x8b\x3e\xf8\xc1\x22\xd9\x63\x55\x14\xce\xd5\x65\xfe"));
}

TEST(hmac_sha256_binary_key) {
  // SigV4 派生密钥是 32B 二进制（含 NUL），必须不作字符串截断
  std::string key;
  for (int i = 0; i < 32; ++i) key.push_back(static_cast<char>(i));
  auto a = hmacSha256Raw(key, "x");
  CHECK_EQ(a.size(), size_t(32));
  CHECK_EQ(a, hmacSha256Raw(key, "x"));
}

int main() {
  RUN(sha256_fips_vectors);
  RUN(hmac_sha256_rfc4231);
  RUN(hmac_sha256_binary_key);
  std::printf("crypto_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：`cmake -S . -B build && cmake --build build -j4 --target crypto_test 2>&1 | tail`
预期：FAIL 链接错误（`crypto_test` 未在 CMakeLists 注册，先失败）。

- [ ] **步骤 3：注册测试并修正注释**

在 `CMakeLists.txt` 的 `im_add_unit_test(media_store_test ...)` 之后加：
```cmake
im_add_unit_test(crypto_test tests/crypto_test.cpp)
```
修正 `src/util.cpp:3` 注释：把「PBKDF2-SHA256（OpenSSL，迭代 10 万次）」改为「PBKDF2-SHA256（本文件手写 SHA256/HMAC-SHA256 实现，不依赖 OpenSSL）」。

- [ ] **步骤 4：运行测试验证通过**

运行：`cmake -S . -B build && cmake --build build -j4 && ctest --test-dir build -R crypto_test --output-on-failure`
预期：PASS 3/3。

- [ ] **步骤 5：Commit**

```bash
git add src/util.cpp tests/crypto_test.cpp CMakeLists.txt
git commit -m "test: 补 SHA256/HMAC RFC 向量单测 + 修正 util 注释（非 OpenSSL）"
```

---

### 任务 2：SigV4 预签名纯函数 + 向量单测

**文件：**
- 创建：`src/sigv4.hpp`、`src/sigv4.cpp`
- 创建：`tests/sigv4_test.cpp`
- 修改：`CMakeLists.txt`、`CMakeLists.txt:104`（`UNIT_TEST_SOURCES` 加 `src/sigv4.cpp`）

- [ ] **步骤 1：编写失败的测试**

```cpp
// tests/sigv4_test.cpp
#include "test_util.hpp"
#include "sigv4.hpp"
using namespace im;

TEST(sigv4_presign_get_deterministic) {
  SigV4Config cfg{"minio.example.com", "us-east-1", "mybucket",
                  "AKIAIOSFODNN7EXAMPLE",
                  "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", true};
  SigV4Request req{"GET", "media/1001/2026/10/ab.jpg",
                   /*amzDate=*/"20261001T120000Z", /*expiresSec=*/900};
  auto url = sigV4Presign(cfg, req);
  // 确定性：同输入同输出，且含必备 query
  CHECK(url.find("X-Amz-Algorithm=AWS4-HMAC-SHA256") != std::string::npos);
  CHECK(url.find("X-Amz-Credential=") != std::string::npos);
  CHECK(url.find("X-Amz-Date=20261001T120000Z") != std::string::npos);
  CHECK(url.find("X-Amz-Expires=900") != std::string::npos);
  CHECK(url.find("X-Amz-SignedHeaders=host") != std::string::npos);
  CHECK(url.find("X-Amz-Signature=") != std::string::npos);
  CHECK_EQ(url, sigV4Presign(cfg, req));
}

TEST(sigv4_utf8_key_encoding) {
  SigV4Config cfg{"h", "us-east-1", "b", "AK", "SK", true};
  SigV4Request req{"PUT", "media/1001/相册 图.jpg", "20261001T120000Z", 900};
  auto url = sigV4Presign(cfg, req);
  // 空格必须 %20 不是 +；UTF-8 逐字节百分号编码
  CHECK(url.find("%20") != std::string::npos);
  CHECK(url.find('+') == std::string::npos);
  CHECK(url.find("%E7%9B%B8") != std::string::npos); // "相"
}

TEST(sigv4_scope_and_host) {
  SigV4Config cfg{"minio.example.com", "us-east-1", "mybucket", "AK", "SK", true};
  SigV4Request req{"PUT", "k.png", "20261001T120000Z", 600};
  auto url = sigV4Presign(cfg, req);
  CHECK(url.find("s3%2Faws4_request") != std::string::npos); // Credential scope
  CHECK(url.rfind("http://minio.example.com/mybucket/k.png", 0) == 0); // path style
}

TEST(sigv4_expires_overflow_rejected) {
  SigV4Config cfg{"h", "us-east-1", "b", "AK", "SK", true};
  SigV4Request req{"GET", "k", "20261001T120000Z", 604801}; // >7 天上限
  CHECK(sigV4Presign(cfg, req).empty());
}

int main() {
  RUN(sigv4_presign_get_deterministic);
  RUN(sigv4_utf8_key_encoding);
  RUN(sigv4_scope_and_host);
  RUN(sigv4_expires_overflow_rejected);
  std::printf("sigv4_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：`cmake -S . -B build && cmake --build build -j4 --target sigv4_test 2>&1 | tail`
预期：FAIL，`sigv4.hpp` 不存在。

- [ ] **步骤 3：实现最少代码**

```cpp
// src/sigv4.hpp
#pragma once
#include <cstdint>
#include <string>
namespace im {

struct SigV4Config {
  std::string host;        // endpoint host，如 minio.example.com（不含 scheme）
  std::string region;      // 如 us-east-1
  std::string bucket;
  std::string accessKey;
  std::string secretKey;
  bool pathStyle = true;   // MinIO 用 path style
  std::string scheme = "http"; // 生产 https，本地 http
};

struct SigV4Request {
  std::string method;      // GET/PUT/HEAD/DELETE
  std::string objectKey;   // 不含 bucket，不含前导 /
  std::string amzDate;     // UTC yyyyMMddTHHmmssZ
  int64_t expiresSec;      // 1..604800
};

// 生成预签名 URL；参数非法（method/expires/空字段）返回空串。
// 纯函数，无副作用，可单测。
std::string sigV4Presign(const SigV4Config& cfg, const SigV4Request& req);

// 单测辅助：AWS/S3 URI 编码（对 unreserved 集外的字符做 %XX，空格→%20）
std::string sigV4UriEncode(const std::string& s, bool encodeSlash);

// 单测辅助：给定 UTC yyyyMMddTHHmmssZ 取 dateStamp(yyyyMMdd)
std::string sigV4DateStamp(const std::string& amzDate);

} // namespace im
```

`src/sigv4.cpp` 关键逻辑（完整实现）：
```cpp
#include "sigv4.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace im {
namespace {
bool isUnreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

// 派生签名密钥：HMAC 链，中间值是二进制，不能转 hex
std::string signingKey(const std::string& secret, const std::string& date,
                       const std::string& region, const std::string& service) {
  std::string k1 = hmacSha256Raw("AWS4" + secret, date);
  std::string k2 = hmacSha256Raw(k1, region);
  std::string k3 = hmacSha256Raw(k2, service);
  return hmacSha256Raw(k3, "aws4_request");
}

std::string toHex(const std::string& raw) {
  static const char* h = "0123456789abcdef";
  std::string o;
  for (unsigned char c : raw) { o += h[c >> 4]; o += h[c & 0xF]; }
  return o;
}
} // namespace

std::string sigV4UriEncode(const std::string& s, bool encodeSlash) {
  static const char* h = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isUnreserved(c) || (c == '/' && !encodeSlash)) {
      o.push_back(static_cast<char>(c));
    } else {
      o.push_back('%'); o.push_back(h[c >> 4]); o.push_back(h[c & 0xF]);
    }
  }
  return o;
}

std::string sigV4DateStamp(const std::string& amzDate) {
  return amzDate.size() >= 8 ? amzDate.substr(0, 8) : std::string();
}

std::string sigV4Presign(const SigV4Config& cfg, const SigV4Request& req) {
  if (cfg.host.empty() || cfg.region.empty() || cfg.bucket.empty() ||
      cfg.accessKey.empty() || cfg.secretKey.empty()) return "";
  if (req.method != "GET" && req.method != "PUT" &&
      req.method != "HEAD" && req.method != "DELETE") return "";
  if (req.expiresSec <= 0 || req.expiresSec > 604800) return "";
  const std::string dateStamp = sigV4DateStamp(req.amzDate);
  if (dateStamp.empty() || req.amzDate.size() != 16) return "";

  const std::string service = "s3";
  // Canonical URI：每个 path 段编码，保留 '/'
  std::string canonicalUri = "/" + sigV4UriEncode(cfg.bucket, true) + "/" +
                             sigV4UriEncode(req.objectKey, false);
  // query 参数（按 key 排序：X-Amz-* 自然有序）
  const std::string scope = dateStamp + "/" + cfg.region + "/" + service + "/aws4_request";
  std::vector<std::pair<std::string, std::string>> q = {
      {"X-Amz-Algorithm", "AWS4-HMAC-SHA256"},
      {"X-Amz-Credential", sigV4UriEncode(cfg.accessKey + "/" + scope, true)},
      {"X-Amz-Date", req.amzDate},
      {"X-Amz-Expires", std::to_string(req.expiresSec)},
      {"X-Amz-SignedHeaders", "host"},
  };
  std::sort(q.begin(), q.end());
  std::string canonicalQuery;
  for (size_t i = 0; i < q.size(); ++i) {
    if (i) canonicalQuery += "&";
    canonicalQuery += sigV4UriEncode(q[i].first, true) + "=" + q[i].second;
  }
  const std::string canonicalHeaders = "host:" + cfg.host + "\n";
  const std::string canonicalRequest =
      req.method + "\n" + canonicalUri + "\n" + canonicalQuery + "\n" +
      canonicalHeaders + "\n" + "host" + "\n" + "UNSIGNED-PAYLOAD";
  const std::string stringToSign =
      "AWS4-HMAC-SHA256\n" + req.amzDate + "\n" + scope + "\n" +
      sha256Hex(canonicalRequest);
  const std::string signature =
      toHex(hmacSha256Raw(signingKey(cfg.secretKey, dateStamp, cfg.region, service),
                          stringToSign));
  return cfg.scheme + "://" + cfg.host + canonicalUri + "?" + canonicalQuery +
         "&X-Amz-Signature=" + signature;
}

} // namespace im
```

- [ ] **步骤 4：运行测试验证通过**

在 `CMakeLists.txt` 的 `UNIT_TEST_SOURCES` 加 `src/sigv4.cpp`，并加 `im_add_unit_test(sigv4_test tests/sigv4_test.cpp)`。
运行：`cmake -S . -B build && cmake --build build -j4 && ctest --test-dir build -R sigv4_test --output-on-failure`
预期：PASS 4/4。

- [ ] **步骤 5：Commit**

```bash
git add src/sigv4.hpp src/sigv4.cpp tests/sigv4_test.cpp CMakeLists.txt
git commit -m "feat: S3 SigV4 预签名纯函数（复用自研 SHA256/HMAC）"
```

---

### 任务 3：media 表 + 迁移 + media_id 列

**文件：**
- 创建：`db/migrate_media.sql`
- 修改：`db/schema.sql`

- [ ] **步骤 1：编写失败的验证（SQL）**

```sql
-- 验证：迁移后 media 表存在且 messages 有 media_id 列
SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema='IMS_reactor_db' AND table_name='media';
SELECT COUNT(*) FROM information_schema.columns
 WHERE table_schema='IMS_reactor_db' AND table_name='messages' AND column_name='media_id';
```
预期：当前均为 0（失败）。

- [ ] **步骤 2：运行验证失败**

运行：`MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db -N -e "<上方SQL>"`
预期：`0` 与 `0`。

- [ ] **步骤 3：编写迁移与 schema**

```sql
-- db/migrate_media.sql（幂等，仿 migrate_msg_type.sql 的 information_schema 守卫）
-- 媒体表：对象存储引用与元数据
CREATE TABLE IF NOT EXISTS media (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  file_id       CHAR(32)     NOT NULL,
  uploader_id   BIGINT UNSIGNED NOT NULL,
  object_key    VARCHAR(255) NOT NULL,
  file_name     VARCHAR(255) NOT NULL,
  mime_type     VARCHAR(64)  NOT NULL,
  file_size     BIGINT UNSIGNED NOT NULL DEFAULT 0,
  width         INT          NOT NULL DEFAULT 0,
  height        INT          NOT NULL DEFAULT 0,
  duration      INT          NOT NULL DEFAULT 0,
  thumb_key     VARCHAR(255) NOT NULL DEFAULT '',
  status        TINYINT      NOT NULL DEFAULT 0,  -- 0=PENDING 1=UPLOADED 2=COMMITTED 3=GC
  created_at    BIGINT       NOT NULL DEFAULT 0,
  UNIQUE KEY uk_file_id (file_id),
  UNIQUE KEY uk_object_key (object_key),
  KEY idx_uploader (uploader_id),
  KEY idx_status_created (status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- messages.media_id（幂等）
SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='messages' AND column_name='media_id');
SET @s := IF(@c=0,
  'ALTER TABLE messages ADD COLUMN media_id BIGINT UNSIGNED NULL, ADD KEY idx_media (media_id)',
  'SELECT ''skip media_id''');
PREPARE stmt FROM @s; EXECUTE stmt; DEALLOCATE PREPARE stmt;
```

`db/schema.sql`：在 `messages` 建表定义里加 `media_id BIGINT UNSIGNED NULL`（`msg_type` 之后），并在文件末尾追加完整 `CREATE TABLE IF NOT EXISTS media (...)`（与迁移同定义）。

- [ ] **步骤 4：运行验证通过**

运行：在开发库执行 `MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_media.sql` 两遍（第二遍验证幂等），再跑步骤 1 的 SQL。
预期：两列均 `1`。

- [ ] **步骤 5：Commit**

```bash
git add db/migrate_media.sql db/schema.sql
git commit -m "feat: media 表 + messages.media_id（幂等迁移）"
```

---

### 任务 4：MediaRepository（media 表存取）

**文件：**
- 创建：`src/media_repository.hpp`
- 创建：`src/mysql_media_repository.{hpp,cpp}`
- 修改：`CMakeLists.txt`

- [ ] **步骤 1：编写失败的测试**

本仓库仓储层无独立单测（依赖真库，与 `MySqlMessageRepository` 同模式）。以**开发库集成验证**替代，测试脚本临时：
```cpp
// /tmp/media_repo_check.cpp（不入库）
auto repo = std::make_shared<MySqlMediaRepository>(host, port, user, pwd, db);
MediaRecord m;
m.fileId = "0123456789abcdef0123456789abcdef";
m.uploaderId = 1; m.objectKey = "media/1/2026/10/x.png";
m.fileName = "x.png"; m.mimeType = "image/png"; m.fileSize = 70;
CHECK(repo->insertPending(m));
auto got = repo->findByFileId(m.fileId);
CHECK(got && got->status == 0 && got->objectKey == m.objectKey);
CHECK(repo->commit(m.fileId, 70, 1, 1, 0));
CHECK(repo->findByFileId(m.fileId)->status == 2);
CHECK(repo->markDeleted(m.fileId));  // status=3
```

- [ ] **步骤 2：运行测试验证失败**

运行：`g++ -std=c++17 -I src /tmp/media_repo_check.cpp src/mysql_media_repository.cpp src/mysql_client.cpp src/util.cpp src/log.cpp -lmysqlclient ...`
预期：编译失败（类不存在）。

- [ ] **步骤 3：实现最少代码**

```cpp
// src/media_repository.hpp
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace im {

struct MediaRecord {
  int64_t id = 0;
  std::string fileId;
  int64_t uploaderId = 0;
  std::string objectKey;
  std::string fileName;
  std::string mimeType;
  int64_t fileSize = 0;
  int width = 0, height = 0, duration = 0;
  std::string thumbKey;
  int status = 0;          // 0 PENDING 1 UPLOADED 2 COMMITTED 3 GC
  int64_t createdAt = 0;
};

enum MediaStatus { kMediaPending = 0, kMediaUploaded = 1, kMediaCommitted = 2, kMediaGc = 3 };

class MediaRepository {
 public:
  virtual ~MediaRepository() = default;
  virtual bool ok() const = 0;
  virtual bool insertPending(const MediaRecord& m) = 0;
  virtual std::optional<MediaRecord> findByFileId(const std::string& fileId) = 0;
  // commit：PENDING→COMMITTED，写入以 HEAD 为准的 size 与元数据；幂等（已 COMMITTED 返回 true）
  virtual bool commit(const std::string& fileId, int64_t size, int width, int height, int duration) = 0;
  virtual bool markDeleted(const std::string& fileId) = 0;
  // GC：返回超期 PENDING（< olderThan）或已删记录，供清理对象
  virtual std::vector<MediaRecord> findPendingOlderThan(int64_t olderThanSec, int limit) = 0;
  // 是否被任何消息引用
  virtual bool isReferenced(const std::string& fileId) = 0;
};
} // namespace im
```

`src/mysql_media_repository.{hpp,cpp}`：仿 `MySqlMessageRepository` 结构（`MySqlClient client_; std::mutex mutex_;`，`stmtExec`/`query` + `rowToMedia`，`kCols=13`）。SQL：
- insert：`INSERT INTO media (file_id,uploader_id,object_key,file_name,mime_type,file_size,width,height,duration,thumb_key,status,created_at) VALUES (?,?,?,?,?,?,?,?,?,?,0,?)`
- find：`SELECT id,file_id,uploader_id,object_key,file_name,mime_type,file_size,width,height,duration,thumb_key,status,created_at FROM media WHERE file_id=?`
- commit（幂等，仅 PENDING 更新）：`UPDATE media SET status=2,file_size=?,width=?,height=?,duration=? WHERE file_id=? AND status=0`；若 affected=0 且已存在 status>=2 → 视为成功。
- isReferenced：`SELECT 1 FROM messages WHERE media_id=(SELECT id FROM media WHERE file_id=?) LIMIT 1`。

- [ ] **步骤 4：运行验证通过**

运行：编译并向开发库跑 `/tmp/media_repo_check.cpp`。
预期：全 CHECK 通过；`media` 表出现该记录并最终 status=3；用完按 file_id 精确清理。

- [ ] **步骤 5：Commit**

```bash
git add src/media_repository.hpp src/mysql_media_repository.hpp src/mysql_media_repository.cpp CMakeLists.txt
git commit -m "feat: MediaRepository（media 表存取，幂等 commit）"
```

---

### 任务 5：ObjectStore 接口 + MinIO 实现（预签名 + libcurl）

**文件：**
- 创建：`src/object_store.hpp`
- 创建：`src/minio_object_store.{hpp,cpp}`
- 创建：`tests/minio_object_store_test.cpp`
- 修改：`CMakeLists.txt`（源文件 + 单测 + libcurl）

- [ ] **步骤 1：编写失败的测试**

```cpp
// tests/minio_object_store_test.cpp —— 纯逻辑（不连真机）
#include "test_util.hpp"
#include "minio_object_store.hpp"
using namespace im;

TEST(minio_presign_put_and_get_shape) {
  MinioConfig c;
  c.endpointHost = "minio.example.com"; c.region = "us-east-1";
  c.bucket = "media"; c.accessKey = "AK"; c.secretKey = "SK"; c.pathStyle = true;
  MinioObjectStore s(c);
  auto put = s.presignPut("media/1/a.png", 900, "20261001T120000Z");
  auto get = s.presignGet("media/1/a.png", 3600, "20261001T120000Z");
  CHECK(put.url.rfind("http://minio.example.com/media/media/1/a.png?", 0) == 0);
  CHECK(put.url.find("X-Amz-Expires=900") != std::string::npos);
  CHECK(get.url.find("X-Amz-Expires=3600") != std::string::npos);
  CHECK(put.expiresIn == 900 && get.expiresIn == 3600);
}

TEST(minio_disabled_when_config_missing) {
  MinioConfig c; // 空配置
  MinioObjectStore s(c);
  CHECK(!s.enabled());
  CHECK(s.presignPut("k", 900, "20261001T120000Z").url.empty());
}

int main() {
  RUN(minio_presign_put_and_get_shape);
  RUN(minio_disabled_when_config_missing);
  std::printf("minio_object_store_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
```

- [ ] **步骤 2：运行测试验证失败**

运行：`cmake --build build -j4 --target minio_object_store_test 2>&1 | tail`
预期：FAIL（头文件不存在）。

- [ ] **步骤 3：实现最少代码**

```cpp
// src/object_store.hpp
#pragma once
#include <cstdint>
#include <string>
namespace im {

struct PresignedUrl { std::string url; int64_t expiresIn = 0; };
struct ObjectInfo { bool exists = false; uint64_t size = 0; };

class ObjectStore {
 public:
  virtual ~ObjectStore() = default;
  virtual bool enabled() const = 0;
  // ttlSec 由调用方给（PUT 900s / GET 3600s）；amzDate 便于单测固定时钟
  virtual PresignedUrl presignPut(const std::string& key, int64_t ttlSec,
                                  const std::string& amzDate = "") = 0;
  virtual PresignedUrl presignGet(const std::string& key, int64_t ttlSec,
                                  const std::string& amzDate = "") = 0;
  virtual ObjectInfo headObject(const std::string& key) = 0;   // 服务端预签名+libcurl
  virtual bool deleteObject(const std::string& key) = 0;       // 服务端预签名+libcurl
};
} // namespace im
```

`src/minio_object_store.{hpp,cpp}`：
```cpp
// 头
struct MinioConfig {
  std::string endpointHost;  // 不含 scheme
  std::string scheme = "http";
  std::string region = "us-east-1";
  std::string bucket;
  std::string accessKey;
  std::string secretKey;
  bool pathStyle = true;
};
class MinioObjectStore : public ObjectStore {
 public:
  explicit MinioObjectStore(MinioConfig c);
  bool enabled() const override { return enabled_; }
  PresignedUrl presignPut(const std::string& key, int64_t ttlSec,
                          const std::string& amzDate = "") override;
  PresignedUrl presignGet(const std::string& key, int64_t ttlSec,
                          const std::string& amzDate = "") override;
  ObjectInfo headObject(const std::string& key) override;
  bool deleteObject(const std::string& key) override;
 private:
  MinioConfig cfg_;
  bool enabled_ = false;
  static std::string nowAmzDate();  // UTC yyyyMMddTHHmmssZ
};
```
`.cpp` 要点：
- `nowAmzDate()`：`time(nullptr)` + `gmtime_r` 格式化为 `%Y%m%dT%H%M%SZ`。
- `presignPut/Get`：调 `sigV4Presign(cfg_, {"PUT"/"GET", key, amzDate.empty()?nowAmzDate():amzDate, ttlSec})`；空 URL 时 `enabled_=false` 语义返回空。
- `headObject`（libcurl）：`presignGet(key, 300)` → `curl_easy_setopt(CURLOPT_URL/CURLOPT_NOBODY=1L/CURLOPT_CUSTOMREQUEST,"HEAD")` → 执行；HTTP 200 表示存在，`CURLINFO_CONTENT_LENGTH_DOWNLOAD_T` 取 size；404 返回 `{false,0}`。
- `deleteObject`：`sigV4Presign(..., "DELETE", ...)` → `CURLOPT_CUSTOMREQUEST,"DELETE"` → HTTP 2xx 为成功。
- **libcurl 惰性初始化**：构造里 `curl_global_init(CURL_GLOBAL_DEFAULT)`（进程一次；或用 `std::once_flag`）。

CMake：`find_library(CURL_LIB NAMES curl)`，缺失 FATAL_ERROR；`imserver` 与相关测试 `target_link_libraries(... ${CURL_LIB})`。

- [ ] **步骤 4：运行测试验证通过**

运行：`cmake -S . -B build && cmake --build build -j4 && ctest --test-dir build -R minio_object_store_test --output-on-failure`
预期：PASS 2/2。

- [ ] **步骤 5：Commit**

```bash
git add src/object_store.hpp src/minio_object_store.hpp src/minio_object_store.cpp tests/minio_object_store_test.cpp CMakeLists.txt
git commit -m "feat: ObjectStore 接口 + MinIO 实现（预签名 + libcurl HEAD/DELETE）"
```

---

### 任务 6：MediaService（presign/commit/url 编排 + 权限校验）

**文件：**
- 创建：`src/media_service.{hpp,cpp}`
- 修改：`src/server_config.hpp`（媒体常量）
- 修改：`CMakeLists.txt`

- [ ] **步骤 1：编写失败的测试**

服务编排依赖 DB/对象存储，采用开发库 + 假 ObjectStore 集成验证：
```cpp
// /tmp/media_service_check.cpp（不入库）
class FakeStore : public ObjectStore {
 public:
  bool enabled() const override { return true; }
  PresignedUrl presignPut(const std::string& k, int64_t t, const std::string&) override { return {"http://x/" + k, t}; }
  PresignedUrl presignGet(const std::string& k, int64_t t, const std::string&) override { return {"http://g/" + k, t}; }
  ObjectInfo headObject(const std::string&) override { return {true, 70}; }
  bool deleteObject(const std::string&) override { return true; }
};
// presign → 返回 fileId/uploadUrl；commit → head 通过转 COMMITTED；
// commit 不存在对象（FakeStore 返回 exists=false）→ 失败
```

- [ ] **步骤 2：运行测试验证失败**

运行：编译该临时程序。
预期：编译失败（`MediaService` 不存在）。

- [ ] **步骤 3：实现最少代码**

`src/server_config.hpp` 新增：
```cpp
inline constexpr size_t kMediaFileNameMaxLen = 255;
inline constexpr size_t kMediaSizeMaxBytes = 20 * 1024 * 1024;   // 单文件 20MB
inline constexpr int64_t kMediaPutTtlSec = 900;                  // PUT 15 分钟
inline constexpr int64_t kMediaGetTtlSec = 3600;                 // GET 60 分钟
inline constexpr std::chrono::seconds kMediaWindow(60);
inline constexpr size_t kMediaPresignPerWindow = 30;             // 每用户 30 次/分钟
inline constexpr int64_t kMediaPendingTtlSec = 24 * 3600;        // 未 commit 24h 回收
inline constexpr int kMediaGcBatch = 100;
```

`src/media_service.hpp`：
```cpp
#pragma once
#include "media_repository.hpp"
#include "object_store.hpp"
#include "rate_limiter_store.hpp"
#include <memory>
#include <string>
namespace im {

struct PresignResult { bool ok; int code; std::string msg; std::string fileId; std::string uploadUrl; int64_t expiresIn; };
struct CommitResult { bool ok; int code; std::string msg; };
struct UrlResult { bool ok; int code; std::string msg; std::string url; int64_t expiresIn; };

// 权限校验回调：由 AppServer 注入，判断 user 是否有权访问引用 fileId 的消息
using MediaAccessFn = std::function<bool(int64_t userId, const std::string& fileId)>;

class MediaService {
 public:
  MediaService(std::shared_ptr<MediaRepository> repo,
               std::shared_ptr<ObjectStore> store,
               std::shared_ptr<RateLimiterStore> limiter,
               std::string bucketPathPrefix = "media");

  PresignResult presign(int64_t userId, const std::string& fileName,
                        const std::string& mimeType, int64_t size, const std::string& kind);
  CommitResult commit(int64_t userId, const std::string& fileId, int64_t size,
                      int width, int height, int duration);
  UrlResult url(int64_t userId, const std::string& fileId, const MediaAccessFn& canAccess);
  int64_t gcOnce(); // 清理超期 PENDING：删对象 + 标记（返回条数）
  static bool validMime(const std::string& mime);
  static bool validKind(const std::string& kind);
 private:
  std::shared_ptr<MediaRepository> repo_;
  std::shared_ptr<ObjectStore> store_;
  std::shared_ptr<RateLimiterStore> limiter_;
  std::string prefix_;
  std::string objectKey(int64_t userId, const std::string& fileId, const std::string& mime);
  static std::string randomFileId();  // 32 hex（复用 util 随机字节）
};
} // namespace im
```

关键实现：
- `objectKey`：`prefix_ + "/" + uploaderId + "/" + YYYY + "/" + MM + "/" + fileId + ext`（ext 由 mime 派生白名单：image/png→.png 等；未知→.bin）。
- `randomFileId()`：16 随机字节 → 32 hex（用 `util` 随机源，别用 `rand()`）。
- `presign`：校验 kind∈{image}（V1）、mime 白名单、size∈(0,kMediaSizeMaxBytes]、fileName 长度；限流 `limiter_->allow(userId, kMediaPresignPerWindow, kMediaWindow)`；生成 fileId+objectKey，`repo_->insertPending`；`store_->presignPut`；返回。
- `commit`：`findByFileId` 存在且 `uploaderId==userId`；`headObject` 必须 exists 且 size 与 HEAD 一致（以 HEAD 为准）；`repo_->commit`；幂等。
- `url`：`findByFileId`；`canAccess(userId, fileId)` 必须 true；`store_->presignGet`。
- `gcOnce`：`findPendingOlderThan(now-24h)` → 对每条 `deleteObject` + `markDeleted`。
- `validMime`：`image/png|jpeg|gif|webp`（V2/V3 扩展白名单，V1 仅图片）。

- [ ] **步骤 4：运行验证通过**

运行：编译并跑 `/tmp/media_service_check.cpp`（FakeStore），断言 presign/commit/url/gc 分支。
预期：全通过；用后按 file_id 精确清理。

- [ ] **步骤 5：Commit**

```bash
git add src/server_config.hpp src/media_service.hpp src/media_service.cpp CMakeLists.txt
git commit -m "feat: MediaService（presign/commit/url/gc + 权限回调）"
```

---

### 任务 7：消息层接入 media_id（StoredMessage/save/SELECT/序列化）

**文件：**
- 修改：`src/message_repository.hpp`（`StoredMessage.mediaId` + save 参数）
- 修改：`src/mysql_message_repository.{hpp,cpp}`（`kMsgCols 10→11`、三条 SELECT、rowToMessage、save SQL）
- 修改：`src/app_message.cpp`、`src/app_group.cpp`（落库传 mediaId、下发透传 mediaId）
- 修改：`CMakeLists.txt`（若涉及）

- [ ] **步骤 1：编写失败的测试（biz 层，任务 10 会补全；此处先做编译级）**

```cpp
// 断言：StoredMessage 有 mediaId 字段
StoredMessage m;
m.mediaId = 0;  // 编译失败即红灯
```

- [ ] **步骤 2：运行测试验证失败**

运行：`cmake --build build -j4 2>&1 | grep -m1 mediaId`
预期：编译错误 `no member named 'mediaId'`。

- [ ] **步骤 3：实现最少代码**

`message_repository.hpp`：
```cpp
struct StoredMessage {
  // ...existing...
  int64_t mediaId = 0;   // 0=无媒体；>0 关联 media.id
};
// saveMessage(..., int64_t groupId = 0, const std::string& msgType = "text",
//             int64_t mediaId = 0)
```
`mysql_message_repository.cpp`：
- `kMsgCols 10→11`，注释加 `media_id`。
- 三条 SELECT（fetchOffline/fetchHistory/fetchGroupMessages）列表末尾加 `media_id`。
- `rowToMessage`：`m.mediaId = std::strtoll(row[10], nullptr, 10);`（空/NULL 兜 0）。
- save SQL：列加 `media_id`、占位符 `?`，参数末尾加 `std::to_string(mediaId)`。

`app_message.cpp`：
- `onSend` 的 `msgType=image` 分支改为：读取 `data["mediaId"]`（字符串 fileId）→ 通过 `mediaService_` 校验归属+COMMITTED → 取 `media.id` 作为 `mediaId`，`content` 存 fileId（或空）；**删除 base64/MediaStore 分支**。
- recvData/离线/历史序列化加 `{"mediaId", <fileId>}`。
`app_group.cpp`：`onGroupSend` 同改。

- [ ] **步骤 4：运行测试验证通过**

运行：`cmake --build build -j4 && ctest --test-dir build`
预期：编译通过；现有 ctest 全绿（biz 图片用例此时会红，留待任务 10）。

- [ ] **步骤 5：Commit**

```bash
git add src/message_repository.hpp src/mysql_message_repository.* src/app_message.cpp src/app_group.cpp
git commit -m "feat: 消息层接入 media_id（替换图片 base64 路径）"
```

---

### 任务 8：AppServer 装配 + 3 条协议路由

**文件：**
- 修改：`src/appserver.hpp`（成员 + 3 个 handler 声明）
- 修改：`src/appserver.cpp`（工厂、构造、buildRoutes、GC 调度）
- 修改：`src/app_auth.cpp` 或 `onEnvelope`（鉴权已覆盖，确认 media.* 需登录）

- [ ] **步骤 1：编写失败的测试（biz 层）**

```python
r = send(ws, "media.presign", {"fileName": "a.png", "mimeType": "image/png",
                               "size": 100, "kind": "image"})
check("presign requires login → 401/403", r["data"].get("code") in (401, 403))
```

- [ ] **步骤 2：运行测试验证失败**

运行：`bash scripts/test_biz.sh 2>&1 | grep -i presign`
预期：FAIL（未知指令，无响应）。

- [ ] **步骤 3：实现最少代码**

`appserver.hpp`：加成员
```cpp
std::shared_ptr<ObjectStore> objectStore_;
std::unique_ptr<MediaService> mediaService_;
std::shared_ptr<MediaRepository> mediaRepo_;
void onMediaPresign(const std::shared_ptr<Session>& s, const json& d);
void onMediaCommit(const std::shared_ptr<Session>& s, const json& d);
void onMediaUrl(const std::shared_ptr<Session>& s, const json& d);
```
`appserver.cpp`：
- 构造里从环境变量读 `IM_S3_*` 建 `MinioObjectStore`（缺失则 store 不 enabled，媒体功能降级）、`MySqlMediaRepository`、`imgLimiter_` 复用为 media 限流（或新增 `mediaLimiter_` 用 `mrate:` 前缀）、`MediaService`。
- `buildRoutes` 加 `{"media.presign", &AppServer::onMediaPresign}` 等三条。
- `onEnvelope` 登录门禁：`media.*` 必须已登录（现有未登录白名单不含它们，自动覆盖）。
- purge 轮内接 `mediaService_->gcOnce()`（沿用 last-run 判定；替换原 `MediaStore::gcExpired` 调用）。
- 权限回调 `canAccess`：查 `msgRepo_` 是否存在含该 media 且当前用户可达的消息（单聊 from/to；群聊成员窗口）。

- [ ] **步骤 4：运行测试验证通过**

运行：`bash scripts/test_biz.sh 2>&1 | tail -3`
预期：presign 未登录被拒；登录后返回 `fileId/uploadUrl`。

- [ ] **步骤 5：Commit**

```bash
git add src/appserver.hpp src/appserver.cpp
git commit -m "feat: 媒体协议路由装配（media.presign/commit/url + GC）"
```

---

### 任务 9：部署面（MinIO 服务、libcurl、下线旧图片静态）

**文件：**
- 修改：`docker-compose.yml`（加 minio 服务 + imserver 环境变量）
- 修改：`Dockerfile`（builder 装 `libcurl4-openssl-dev`）
- 修改：`nginx/im.conf`（移除 `/uploads/` 静态）
- 修改：`compose.dev.yaml`（本地开发加 minio）

- [ ] **步骤 1：编写验证**

```bash
docker compose -f docker-compose.yml config >/dev/null && echo compose-ok
docker compose -f compose.dev.yaml config >/dev/null && echo dev-ok
```

- [ ] **步骤 2：运行验证失败**

预期：minio 服务不存在（config 里无 minio）。

- [ ] **步骤 3：最少改动**

`docker-compose.yml` 加：
```yaml
  minio:
    image: minio/minio:latest
    command: ["server", "/data", "--console-address", ":9001"]
    environment:
      MINIO_ROOT_USER: ${IM_S3_ACCESS_KEY:-minioadmin}
      MINIO_ROOT_PASSWORD: ${IM_S3_SECRET_KEY:-minioadmin}
    volumes: [minio_data:/data]
    networks: [imnet]
    # 内网仅供 imserver；不做公网端口（生产换 OSS）
```
`imserver` 环境加：`IM_S3_ENDPOINT: minio:9000`、`IM_S3_REGION: us-east-1`、`IM_S3_BUCKET: media`、`IM_S3_ACCESS_KEY/SECRET_KEY`、`IM_S3_PATH_STYLE: "true"`。
`Dockerfile` builder 段 `apt-get install` 加 `libcurl4-openssl-dev`；runtime 段确认已有 `libcurl4`（`curl` 包已带）。
`nginx/im.conf` 删除 `location /uploads/` 两段（图片不再本地托管）。
`compose.dev.yaml` 加同款 minio（映射 `127.0.0.1:9000:9000` 便于本地直连测试）。
顶层 `volumes` 加 `minio_data`。

- [ ] **步骤 4：运行验证通过**

运行：`docker compose -f docker-compose.yml config >/dev/null && docker compose -f compose.dev.yaml config >/dev/null && echo OK`
预期：OK；`docker compose -f compose.dev.yaml up -d minio` 后 `curl 127.0.0.1:9000/minio/health/live` 返回 200。

- [ ] **步骤 5：Commit**

```bash
git add docker-compose.yml compose.dev.yaml Dockerfile nginx/im.conf
git commit -m "feat: MinIO 服务接入 + libcurl 依赖 + 下线本地图片静态"
```

---

### 任务 10：biz 用例改造 + 负例矩阵 + 全量回归

**文件：**
- 修改：`scripts/biz_test.py`

- [ ] **步骤 1：编写失败的测试**

把原图片用例（`PNG_1X1_B64`/`send_slow` burst 段）替换为对象存储流：
```python
# presign → 真机 PUT 到 MinIO → commit → msg.send(mediaId) → 收 msg.recv 带 mediaId
# 查看方 media.url → 鉴权 → GET 下载校验字节一致
r = send(ws_a, "media.presign", {"fileName": "p.png", "mimeType": "image/png",
                                 "size": len(png_bytes), "kind": "image"})
url = r["data"]["data"]["uploadUrl"]; fid = r["data"]["data"]["fileId"]
put(url, png_bytes)                       # urllib PUT
r = send(ws_a, "media.commit", {"fileId": fid, "size": len(png_bytes),
                                "width": 1, "height": 1})
check("commit ok", r["data"]["code"] == 0)
r = send_msg(ws_a, B, "", "img-1", msg_type="image", media_id=fid)
check("image send ok", r["data"]["code"] == 0)
r = json.loads(ws_b.recv())
check("recv has mediaId", r["data"].get("mediaId") == fid)
```

补充负例：
```python
# 未登录 presign 拒绝；伪造 fileId 的 object_key commit 失败；
# commit 未上传对象失败；重复 commit 幂等；media.url 越权拒绝（非消息相关方）；
# 群聊图片非成员取 url 拒绝；过期 PUT URL 上传失败
```

- [ ] **步骤 2：运行测试验证失败**

运行：`IM_REDIS_ENABLED=1 IM_REDIS_PORT=6379 bash scripts/test_biz.sh 2>&1 | tail -5`
预期：新用例 FAIL（旧路径行为已移除）。

- [ ] **步骤 3：实现测试改造**

- `send_msg` 增加 `media_id` 参数（写入 `data["mediaId"]`）。
- 新增 `put(url,bytes)` 用 `urllib.request`（`method="PUT"`）。
- `send(ws, type, data)` 若不存在则加通用发送辅助。
- 清理段 `used` 列表沿用现有账号清理；新增的 presign 记录由 `media` 表的 test 账号关联删除（按 `uploader_id` 精确删）。

- [ ] **步骤 4：运行测试验证通过**

运行：
```bash
IM_REDIS_ENABLED=1 IM_REDIS_PORT=6379 bash scripts/test_biz.sh 2>&1 | tail -3
ctest --test-dir build 2>&1 | tail -3
```
预期：全绿；负例全部按预期码返回。

- [ ] **步骤 5：Commit**

```bash
git add scripts/biz_test.py
git commit -m "test: 图片用例改走对象存储流 + 媒体负例矩阵"
```

---

### 任务 11：文档与协议更新

**文件：**
- 修改：`docs/protocol.md`（media.* 指令、msg.send mediaId、废弃 base64 语义）
- 修改：`README.md`（图片/媒体特性、MinIO、配置项）

- [ ] **步骤 1：编写验证**

```bash
grep -c "media.presign" docs/protocol.md   # 期望 >=1
grep -c "media_url\|IM_S3_" README.md       # 期望 >=1
```

- [ ] **步骤 2：运行验证失败**

预期：均为 0。

- [ ] **步骤 3：实现文档改动**

- `protocol.md`：指令表加 `media.presign/commit/url`；`msg.send` 说明 `mediaId`；原「图片消息」一节改写为「媒体消息（对象存储）」并移除 base64/`/uploads/` 描述；版本史加 v3.0。
- `README.md`：特性行更新；快速开始补 `IM_S3_*`；测试段数字更新。

- [ ] **步骤 4：运行验证通过**

运行：重新 grep，>=1。

- [ ] **步骤 5：Commit**

```bash
git add docs/protocol.md README.md
git commit -m "docs: 媒体系统协议与部署文档（v3.0）"
```

---

## 自检

**规格覆盖度：**
- §2 控制面/数据面 → 任务 8（路由）+ 任务 10（客户端流）
- §3 决策清单 → 全部任务遵循
- §3.1 HEAD/DELETE 走预签名+libcurl → 任务 5
- §4 数据模型 → 任务 3/4/7
- §5 协议扩展 → 任务 8/10/11
- §6 状态机/GC → 任务 4（状态）+ 任务 6（gcOnce）+ 任务 8（调度）
- §7 权限 → 任务 6（canAccess）+ 任务 8（注入）
- §8 ObjectStore → 任务 5；§8.1 SigV4 → 任务 2；§8.2 配置 → 任务 8/9
- §9 测试 → 任务 1/2/4/5/6/10
- §10 容量 → 任务 6（size 上限）；（配额/告警属 V2，规格已注明）
- §11 分期 V1 → 本计划；§12 迁移 → 任务 7/9/10

**占位符扫描：** 无 TODO/待定；每步含代码/命令/预期。

**类型一致性：** `MediaRecord.fileId`(string) / `mediaId`(int64，DB id) 分工明确；`ObjectStore::presignPut(key,ttl,amzDate)` 签名在任务 5 定义、任务 6 调用一致；`MediaService::presign(...,kind)` 在任务 6 定义、任务 8 调用一致。

**已知限制（非缺陷，V1 刻意）：** 服务端不生成缩略图（客户端出）；不批量签名（V2）；单文件 20MB 上限；OSS 未适配（V4）。

**V1 权限/GC 的简化边界（V2 补全）：**
- `canAccess` 在 V1 取"存在任一 from/to 或群成员引用的消息"**保守放行**；群聊"消息可见窗口（入群/退群时间）"的精确判定属 V2。
- `gcOnce` 在 V1 只回收**超期 PENDING**；"COMMITTED 但无消息引用"的回收依赖任务 4 的 `isReferenced`，V1 仅提供接口与单测，**实际调度在 V2 开启**（避免 V1 过度删除尚未发送但已 commit 的对象）。

## 环境适配裁定（2026-10-01，已批准，不改变架构决策）

**libcurl / CMake：**
- 优先 `find_package(CURL)`；失败则读环境变量 `IM_CURL_ROOT`（前缀含 `include/` 与 `lib/`）查找头与库，封装为导入目标 `CURL::libcurl`。
- **严禁**把 `/tmp/opencode/curlroot` 等机器特定路径写入仓库仓库文件。
- 两种方式都失败 → CMake `FATAL_ERROR` 并说明如何设置 `IM_CURL_ROOT`。
- 本机执行：`export IM_CURL_ROOT=/tmp/opencode/curlroot`（已备好 `usr/include/x86_64-linux-gnu/curl/curl.h` 与 `usr/lib/x86_64-linux-gnu/libcurl.so`）。运行时经系统 `libcurl.so.4` 加载，本机无需 `LD_LIBRARY_PATH`。

**MinIO 集成测试：**
- 本机用静态 MinIO 二进制：`http://127.0.0.1:9000`，bucket `media`，`minioadmin/minioadmin`（**仅本地测试，禁提交仓库**，经环境变量注入）。
- 任务 5/6/10 的真机集成测试连本地 MinIO。
- 任务 9 仍保留 compose 的 minio 服务定义（标准开发/CI 用），但**本机不验证其启动**（镜像拉取被本地网络拒），只做 `docker compose config` 语法/完整性检查。
- 文档必须区分"静态二进制 MinIO 集成测试"与"Compose MinIO 环境验证"，不得混称。

**测试报告要求**（每项独立记录，未执行的标"未验证"，不得互相替代）：SHA256/HMAC 向量、SigV4 单测、本机静态 MinIO 集成、CMake 系统 CURL 查找、`IM_CURL_ROOT` 回退、Compose 配置检查。
