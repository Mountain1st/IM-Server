# 媒体 commit 内容校验 + COMMITTED 孤儿回收 设计规格

> 状态：**实施中**。范围：任务 ②（审计 P0-3 媒体魔数未接线 + P1-2 孤儿不回收）。
> 修订 `2026-10-01-media-system-design.md`：新增 commit 侧服务端内容校验与孤儿回收（该 spec §1
> 「不引入解码/转码/缩略图生成」仍成立——本规格只解析**头部字节**，不做完整解码）。
> 目标仓库：`IM-Server`（基线 `e25bb82`）。

## 1. 背景（审计已确认）

| # | 缺陷 | 证据 |
|---|---|---|
| 1 | `MediaStore`（PNG/JPEG/GIF/WebP magic+维度、SVG 拒绝、4096 上限）**存在且有单测，但主链路 0 调用**——`media.commit` 只做 HEAD 大小校验，任何字节声明 `image/png` 即可 COMMITTED 并下发下载 URL | `src/media_store.cpp`；`git grep MediaStore src/media_service.cpp` = 0 |
| 2 | `isReferenced()` 零调用方；`gcOnce()` 只扫 PENDING。commit 后未 `msg.send` 的对象永久堆积 | `src/mysql_media_repository.cpp:236`；`src/media_service.cpp:293` |
| 3 | GC 删除对象失败仍把状态标为 GC（不再复扫）→ 永久孤儿 | `src/media_service.cpp:313`（PENDING 路径同病） |

## 2. 决策（已定）

### D1 commit 侧内容校验（P0-3）

对象字节由客户端直传对象存储，服务端 commit 时新增**下载对象前缀**（`ObjectStore::getObject`，上限
`kMediaValidatePrefixBytes = 256KB`），经 `MediaStore::detectKind` 校验：

- 声明 MIME（presign 时白名单已过）必须与**实际 magic 解析结果**一致；
- 实际任一边 >4096 → 拒绝；解析失败（含 JPEG 在前缀内未找到 SOF）→ **fail-closed 拒绝**；
- SVG 由 magic 白名单天然拒绝（`<svg` 扫描为纵深防御）；
- 校验拒绝 → 媒体保持 **PENDING**（400），由既有 PENDING GC 兜底回收；不落 COMMITTED。
- 宽高以**服务端解析值为准**入库（与「size 以 HEAD 为准」同一先例），客户端上报值忽略。
- 前缀下载失败（HEAD 成功但 GET 失败）→ `503`（可重试），不计校验拒绝。

无新增 HTTP/WS 接口；仍不引入解码库（只读头部）。

### D2 孤儿回收（P1-2）

`COMPLETED` 且 `created_at < now - kMediaOrphanTtlSec`（默认 7 天）且**无任何 `messages.media_id` 引用**：

1. CAS `status 2→3` 抢占（防与 commit/send 竞态互踩）；
2. 抢占后**复查** `isReferenced()`：`!=0`（被引用或查询失败）→ 回滚 `3→2`，本轮跳过；
3. 删除对象：失败 → 回滚 `3→2`（**下轮重试，绝不标成已回收后永久跳过**）；
4. 成功 → 保持 GC，计数。

### D3 GC 删除失败统一不"假装成功"

PENDING 路径同样修复：`markDeleted`(0→3) 后删除失败 → 回滚 `3→0`，下轮重试，不计入回收数。

### D4 指标

新增 `mediaCommitRejected`（校验拒绝）、`mediaGcPendingReclaimed`、`mediaGcOrphanReclaimed`、
`mediaGcDeleteFailed`，暴露于 `/health`。

### 已知残余风险（明示）

GC 与并发 `msg.send` 的窗口由「保护期 + 抢占后复查」收敛；单实例进程内两仓储各自持锁、无跨仓储事务，
理论窗口（send 已校验 COMMITTED 但尚未 INSERT）不新增 DB 级互锁，属本规格外（保护期 7 天已使实际触发
不可能：发送必然在 commit 后秒级完成）。

## 3. 验收矩阵（用户已锁定）

| 场景 | 期望 |
|---|---|
| 声明 image/png + 真 PNG | PASS |
| 声明 image/png + 非 PNG | REJECT（400） |
| 声明 image/svg+xml + SVG | REJECT（presign 白名单 400） |
| 真 PNG，MIME 声明错误（如 image/jpeg） | REJECT（400） |
| 尺寸 >4096（PNG 头构造 5000x1） | REJECT（400） |
| 声明大小与 HEAD 不一致 | REJECT（400，既有） |
| COMMITTED + 有消息引用 | 保留 |
| COMMITTED + 无引用 + 超保护期 | 回收（状态 GC + 对象删除） |
| 删除对象失败 | 回滚状态、下轮重试（不永久跳过） |

## 4. 测试

**单元**（`tests/media_service_test.cpp`，fake repo/store，无 MySQL/curl）：
commit 校验 5 例（真/假 magic/MIME 错配/超维度/前缀读失败）；GC 4 例（pending 成功、pending 删失败回滚、
orphan 成功、orphan 被引用回滚、orphan 删失败回滚）。

**业务回归**（`biz_test.py` 媒体段，真实 MinIO）：
负例矩阵 e2e；`created_at` 回拨 + `IM_MEDIA_GC_INTERVAL_SEC=1` 验证 pending/orphan 回收；已引用媒体
经 GC 轮询后保持 COMMITTED。

## 5. 非目标

- 服务端转码/缩略图/exif；完整字节解码（仅前缀头解析）。
- `thumb_key` 链路；多附件；Multipart。
- GC 跨仓储 DB 事务互锁（见 §2 残余风险）。
