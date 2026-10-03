# 媒体 commit 校验 + 孤儿回收 实现计划

> 依赖规格：`docs/superpowers/specs/2026-10-03-media-commit-validation-orphan-gc-design.md`。
> **两个逻辑提交**（故障可分别回滚）：M1 校验接线；M2 孤儿回收。

## M1 media: validate committed objects through MediaStore
- `server_config.hpp`：`kMediaValidatePrefixBytes = 256*1024`。
- `ObjectStore`：新增 `getObject(key, maxBytes)`（前缀下载；截断按成功）；
  `MinioObjectStore` 实现（预签名 GET + libcurl，写回调封顶）。
- `MediaStore`：公开 `mimeOf(MediaKind)`（供比对）。
- `MediaService::commit`：HEAD 后下载前缀 → `detectKind` → MIME 一致性 + 维度上限；
  拒绝保持 PENDING 并计 `mediaCommitRejected`；宽高以解析值为准。
- 指标 + `/health` 输出。
- 测试：`tests/media_service_test.cpp`（fake 依赖）+ `CMakeLists`（UNIT_TEST_SOURCES 加
  `metrics.cpp`）；`biz_test.py` 负例矩阵（真/假 magic、MIME 错配、超维度、SVG 白名单）。
- 文档：`protocol.md` commit 行 + `client_api.md` 媒体节。
- 验收：ctest 全绿；biz 媒体段全绿（既有图片全链路回归内含校验路径）。

## M2 media: reclaim unreferenced committed objects
- `MediaRepository` + `MySqlMediaRepository`：`findCommittedOrphanOlderThan(cutoff,limit)`
  （`status=2 AND created_at<? AND NOT EXISTS(messages.media_id=m.id)`）、
  `markOrphanGc(fileId)`（CAS 2→3）、`revertGc(fileId,toStatus)`（CAS 3→to）。
- `MediaService::gcOnce(now)` → 返回 `GcStats{pendingReclaimed,orphanReclaimed,deleteFailures}`；
  PENDING 删失败回滚 0；ORPHAN 抢占后复查引用、删失败回滚 2。
- `AppServer::purgeLoop`：调用新签名；日志分级。
- 指标 + `/health`。
- 测试：单元 GC 5 例；`biz_test.py` 回收 e2e（created_at 回拨 + 轮询；已引用保持）。
- `scripts/test_biz.sh`：服务端加 `IM_MEDIA_GC_INTERVAL_SEC=1`（GC 仅作用于 >24h/7d 行，
  对常规用例无影响）。
- 文档：规格 §2 决策落入 protocol/client_api（保护期语义）。
- 验收：ctest 全绿；biz off/on 双模式全绿。

## 回归命令
```
IM_CURL_ROOT=/home/jackx/curlroot cmake --build build -j4
IM_CURL_ROOT=/home/jackx/curlroot ctest --test-dir build
IM_MYSQL_PASSWORD=... python3 scripts/biz_test.py        # off
IM_MYSQL_PASSWORD=... IM_DELIVERY_ACK_REQUIRED=1 python3 scripts/biz_test.py  # on
```
