# 媒体系统 v3.0 设计（图片/文件/语音/视频 + 对象存储）

> 状态：已头脑风暴确认，待编写实现计划。
> 决策记录：本文件是 v3.0 媒体子系统的规格（spec），实现计划另见 `docs/superpowers/plans/`。

## 1. 目标与范围

把媒体从「base64 塞进 WebSocket 信封 + 本地盘」升级为「对象存储 + 预签名直传」，并把消息与媒体解耦。V1 仅做图片，后续阶段扩展文件/语音/视频。服务器端**不引入解码/转码/缩略图生成**（客户端提供元数据/缩略图），**不做服务端文件中转**。

### 非目标（全版本）

- 服务端 ffmpeg/转码/服务端缩略图生成。
- 服务端中转上传（B1）。
- 一条消息多附件（`message_media` 关联表）。
- Multipart Upload、断点续传、秒传、文件去重。
- 通用 S3 客户端 / AWS SDK / minio-cpp。
- 服务端文件下载代理（数据面完全绕过 imserver）。

## 2. 架构：控制面 / 数据面分离

```
客户端 ──WS(JSON 信封)──> imserver ──控制面──> media 表
   │                          │
   │ ① media.presign 请求授权 URL
   │ ② 得到短期 PUT URL（object_key 服务端生成）
   ├──────── HTTPS PUT ──────> MinIO/OSS（数据面，字节不经过 imserver）
   │ ③ media.commit 登记（服务端 HEAD 校验）
   └── msg.send(msgType=image, mediaId) ──> 消息只存 media_id
                                            │
   查看历史时：msg 携带 mediaId ─> media.url ─> 短期 GET URL ─> 客户端直连对象存储
```

**核心约束**：imserver 只做鉴权、签名、元数据、消息路由；文件字节流不进入 imserver 的内存/网络链路。控制面复用现有 WebSocket JSON 信封（`type` 字段 + 路由表 + 鉴权 + 限流），**不新增 HTTP 路由**。

## 3. 决策清单（已确认）

| 决策项 | 结论 |
|---|---|
| 对象存储后端 | 抽象 `ObjectStore` 接口；V1 仅实现 MinIO；V4 接 OSS |
| 传输方式 | B2 预签名 URL 客户端直传 |
| 控制通道 | 复用现有 WebSocket JSON 信封 |
| 签名实现 | V1 手写 S3 SigV4，**仅预签名 PUT/GET**；复用现有手写 SHA256/HMAC |
| 密码学依赖 | 不引入 OpenSSL / AWS SDK / minio-cpp |
| OSS 适配 | 推迟到 V4，届时重评估 SDK vs 独立签名 |
| 消息↔媒体关系 | 一条消息至多一个媒体，`messages.media_id` 单列 |
| 数据面 DB 值 | 只存 `object_key`，**永不存预签名 URL** |
| 对象桶可见性 | 私有桶 + 短期签名 URL |
| 缩略图/元数据 | 客户端上传，服务端校验 + 存储 + 转发 |
| 路径风格 | Path Style（MinIO 友好），endpoint/bucket/region/AK/SK 全配置化 |
| 分期 | V1 图片全链路 → V2 权限/批量/GC → V3 全媒体类型 → V4 OSS/生产 |

## 4. 数据模型

### 4.1 新表 `media`（`db/migrate_media.sql`，幂等守卫）

| 字段 | 类型 | 说明 |
|---|---|---|
| id | BIGINT UNSIGNED AUTO_INCREMENT PK | 内部主键 |
| file_id | CHAR(32) NOT NULL UNIQUE | 对外不透明 id（随机，防枚举），协议只暴露它 |
| uploader_id | BIGINT UNSIGNED NOT NULL | 上传者 userId |
| object_key | VARCHAR(255) NOT NULL UNIQUE | 对象存储路径，协议外露风险最低 |
| file_name | VARCHAR(255) NOT NULL | 原始文件名（仅展示，不用于路径） |
| mime_type | VARCHAR(64) NOT NULL | 客户端上报，服务端格式校验 |
| file_size | BIGINT UNSIGNED NOT NULL | 客户端上报，commit 时以 HEAD 为准 |
| width | INT NULL | 图片/视频宽 |
| height | INT NULL | 图片/视频高 |
| duration | INT NULL | 音视频时长（秒） |
| thumb_key | VARCHAR(255) NULL | 缩略图对象键（客户端上传） |
| status | TINYINT NOT NULL | 0=PENDING 1=UPLOADED 2=COMMITTED 3=GC |
| created_at | TIMESTAMP NOT NULL | 创建时间 |

### 4.2 `messages` 加列

```sql
ALTER TABLE messages ADD COLUMN media_id BIGINT UNSIGNED NULL;
```

- text 消息：`media_id = NULL`。
- 媒体消息：`media_id` 指向已 COMMITTED 的 media 记录。
- 不建外键约束（与现有表风格一致，靠应用层保证）。

### 4.3 object_key 命名

`media/<uploader_id>/<YYYY>/<MM>/<32位随机hex>.<ext>`

- **必须唯一随机**：预签名 PUT 有效期内可重复使用，固定键会被覆盖（AWS 官方语义）。禁止 `avatar.jpg` 这类可预测命名。
- 保留原始斜杠语义，不标准化路径。

## 5. 协议扩展（WebSocket JSON 信封）

### 5.1 `media.presign` → `media.presign_res`

请求：
```json
{ "type": "media.presign", "data": {
    "fileName": "photo.jpg", "mimeType": "image/jpeg",
    "size": 2048000, "kind": "image"
} }
```
响应：
```json
{ "type": "media.presign_res", "data": {
    "code": 0, "msg": "ok",
    "data": { "fileId": "<32hex>", "uploadUrl": "https://...", "expiresIn": 900 }
} }
```
- 服务端校验：登录态、mime 白名单、size 上限、kind 白名单、按用户 media 限流。
- 生成 file_id + object_key，落 `media` 行 status=PENDING。
- **上传 PUT URL TTL 10–15 分钟**。

### 5.2 `media.commit` → `media.commit_res`

请求：
```json
{ "type": "media.commit", "data": {
    "fileId": "<32hex>", "size": 2048000,
    "width": 1920, "height": 1080, "duration": 0
} }
```
- 服务端 **HEAD object 验证**：对象存在 + 实际大小匹配（以 HEAD 为准，不信客户端声明）。
- 通过则 status=COMMITTED；不然回 400。
- **幂等**：同一 fileId 重复 commit 返回成功，不新建记录。

### 5.3 `media.url` → `media.url_res`

请求：`{ "type": "media.url", "data": { "fileIds": ["<hex>", ...] } }`（支持批量）
响应：`{ "data": { "urls": { "<fileId>": "https://...", "expiresIn": 3600 } } }`
- **必须先做权限校验**（见 §7），通过才签发。
- **下载 GET URL TTL 30–60 分钟**。
- 客户端按过期时间缓存，避免历史加载反复往返。

### 5.4 `msg.send` 变更

- `msgType`: `"text"`（默认）/ `"image"`（V1）/ 后续 `"file" | "voice" | "video"`。
- 媒体消息：请求携带 `mediaId`（`fileId`；`content` 可空），服务端校验该 media 属当前用户且 COMMITTED。
- **移除**现有 `msgType=image` 的 base64 content 语义。
- `msg.recv` / `msg.offline` / `chat.history_res` / `group.history_res`：媒体消息透传 `msgType` + `mediaId`，**不下发 URL**（URL 由客户端按需调 `media.url` 获取）。

## 6. 状态机与一致性

```
PENDING ──commit + HEAD 成功──> COMMITTED ──被消息引用──> 保留
   │                                 │
   └─超 24h 未 commit─> GC(3)         └─无引用且超期 / 消息删除─> GC(3)
```

- **一致性不使用分布式事务**：`media.commit` 与 `msg.send` 是两个独立幂等操作，允许部分失败（上传成功但发消息失败 → 孤儿），由后台 GC 兜底。
- **孤儿场景全覆盖**：拿到 URL 未上传、上传未 commit、commit 未发消息、消息删了对象还在——均通过状态机 + GC 收敛。
- **GC 策略（应用层，替代现有 mtime 7 天盘上 GC）**：
  1. PENDING 超 24h → 删对象（若存在）+ 标记 GC。
  2. COMMITTED 超期且**无任何消息引用** → 删对象 + 标记 GC。
  3. COMMITTED 有引用 → 按消息保留策略（现有 `IM_MSG_RETENTION_DAYS` 默认 90 天）联动。
- GC 调度复用现有 purge 线程（last-run 判定），**不复用** `MediaStore::gcExpired` 的 mtime 逻辑。

## 7. 访问控制（关键安全点）

`media.url` 签发 GET URL 前，服务端必须验证当前用户**有权访问引用该媒体的消息**，**不能**仅凭知道 `file_id` 放行，**不能**只用 `uploader_id == current_user_id`：

- 单聊：存在一条消息 `(from_user=me OR to_user=me)` 且 `media_id=?`。
- 群聊：存在一条群消息 `group_id=G AND media_id=?`，且 `me` 是 G 的（现/曾）成员且在该消息可见窗口内。
- 无任何可达引用 → 拒绝（fail-closed）。
- 权限校验复用现有联系/群成员/拉黑/会话可达性逻辑。

## 8. ObjectStore 抽象

`src/object_store.hpp`（平铺，遵循仓库 `*_store.hpp` 命名，不引入 `src/storage/` 目录）：

```cpp
struct PresignedUrl { std::string url; int64_t expiresIn; };
struct ObjectInfo { bool exists; uint64_t size; };

class ObjectStore {
 public:
  virtual ~ObjectStore() = default;
  virtual PresignedUrl presignPut(const std::string& key, int64_t ttlSec) = 0;
  virtual PresignedUrl presignGet(const std::string& key, int64_t ttlSec) = 0;
  virtual ObjectInfo   headObject(const std::string& key) = 0;   // 服务端凭证
  virtual bool         deleteObject(const std::string& key) = 0; // 服务端凭证
};
```

V1 唯一实现 `MinioObjectStore`（`src/minio_object_store.{hpp,cpp}`）。`MediaService` 只依赖 `ObjectStore` 接口，MinIO 特有逻辑（path style、endpoint、region）不得泄漏到媒体业务层。

### 8.1 SigV4 实现边界（V1）

只实现 **预签名 PUT / GET**（query-string 认证），不实现完整 S3 客户端：
- 复用 `src/util.cpp` 的 `sha256Raw` / `hmacSha256Raw`（手写，无 OpenSSL）。
- **前置任务**：先为 SHA256/HMAC 补 RFC 4231 / FIPS 180 确定性向量单测（当前零覆盖）。
- **顺带修正** `src/util.cpp:3` 错误注释（声称 OpenSSL，实为手写）。
- 签名流程：CanonicalURI → CanonicalQuery（排序+编码）→ CanonicalHeaders（V1 只签 `host`）→ CanonicalRequest → SHA256 → StringToSign（`s3/aws4_request` scope）→ 派生 SigningKey（4 次 HMAC 链）→ HMAC → hex → 拼接 URL。
- **预签名细节**：`X-Amz-SignedHeaders` 的 payload 用 `UNSIGNED-PAYLOAD`；只签 `host` 时客户端 PUT 可带未签名头（Content-Type 不参与校验）——故 mime 以 media 表存值 + commit HEAD 为准。
- **不写**：header 认证、POST 表单上传、Multipart、ListObjects、OSS V4。
- HEAD/DELETE 用服务端 AK/SK 直接调用，不走预签名。

### 8.2 配置（环境变量）

`IM_OBJECT_STORE`(minio) / `IM_S3_ENDPOINT` / `IM_S3_REGION` / `IM_S3_BUCKET` / `IM_S3_ACCESS_KEY` / `IM_S3_SECRET_KEY` / `IM_S3_PATH_STYLE`(true)。**AK/SK 绝不下发客户端**。

## 9. 测试策略

- **前置单测**：SHA256（FIPS 180 空串/abc）+ HMAC（RFC 4231 TC1–TC7）。
- **SigV4 单测**：自建固定向量（AWS 无集中预签名向量表），断言完整 URL；覆盖 UTF-8 object_key、空格、特殊字符、连续斜杠、跨 UTC 日、过期时间、错误 SK。
- **MinIO 真机集成测试**：PUT/HEAD/GET/DELETE 全链路，验证真实客户端请求与签名的头/参数一致；两者不可互替。
- **业务负例矩阵**：未登录申请授权 401；伪造他人 object_key 拒绝；过期 URL 无法上传；commit 不存在对象失败；重复 commit 不重复建记录；消息发送失败不产生永久孤儿（GC 收敛）；未授权用户获取下载 URL 被拒（单聊/群聊双向）；大文件不进入 imserver 内存链路。
- **回归**：现有 266 用例 + ctest 全绿；历史 103 条 text 消息不受影响。

## 10. 磁盘/容量

- **元数据**：media 行 ~350–500B，百万级 ~350–500MB。非瓶颈。
- **对象数据**：= 用户产图总量，由保留期封顶；用 OSS 时 ECS 本地零增量，自建 MinIO 时 ≈ 产图总量。
- **旧资源**：`uploads/` 目录迁移完成后删除；图片从 base64 存 MySQL 变为原图存对象存储，MySQL 反而变小。
- **必须加**：单用户存储配额（如 500MB）+ 卷/桶使用水位告警（如 >80%）——"频率限流 ≠ 总量上限"。

## 11. 分期与验收

| 期 | 内容 | 验收 |
|---|---|---|
| **V1** | 原语单测 + SigV4 + media 表/迁移 + ObjectStore/MinIO + presign/commit/url + 图片统一迁移 + 权限校验 | 图片可上传/发送/接收/历史加载；负例矩阵全过 |
| V2 | 权限细化（群聊窗口）+ 批量签名 + GC 调度接入 purge | 越权拒绝；孤儿按策略清理 |
| V3 | file/voice/video 消息类型 + 客户端缩略图/元数据校验 | 全媒体类型收发 |
| V4 | OSS 适配（SDK vs 手写重评估）+ CORS/证书/最小权限 + 生产部署 | 阿里云 OSS 上线 |

### V1 实施原则

- 先审计 SHA256/HMAC 接口与测试（**已审计：接口支持二进制密钥，测试为零**），补测试后再写 SigV4。
- 不重写已有密码学代码。
- MinIO 特有逻辑不进媒体业务层。
- 所有新增功能必须有单测 + 真机集成测试。

## 12. 迁移（统一迁移，非并存）

现有 `msgType=image` 的 base64-塞-WS 路径整体替换为预签名直传。**已核实历史数据：103 条消息全为 text，0 条图片**，故数据迁移零负担；改动集中在服务端协议 + 客户端。旧的 local `MediaStore`/`uploads/`/nginx `/uploads/` 静态服务在迁移完成后下线（或保留只读一段时间）。
