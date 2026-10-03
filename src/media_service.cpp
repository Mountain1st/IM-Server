// ============================================================
// MediaService：媒体业务编排（对象存储预签名直传）
// - presign：校验 kind/mime/size/fileName + 按用户限流；生成
//   fileId/objectKey，落 PENDING 行，返回 PUT 预签名 URL
// - commit：HEAD 校验对象存在且大小一致（以 HEAD 为准），PENDING→COMMITTED；
//   已 COMMITTED 幂等返回成功
// - url：权限回调通过后签发短期 GET 预签名 URL
// - gcOnce：回收超期 PENDING（删对象 + 标记），返回处理条数
// 线程：由工作线程调用；repo/limiter 自身线程安全，store 无共享可变状态
// ============================================================
#include "media_service.hpp"

#include "log.hpp"
#include "media_store.hpp"
#include "metrics.hpp"
#include "server_config.hpp"

#include <cstdio>
#include <ctime>
#include <random>
#include <utility>
#include <vector>

namespace im {
namespace {

// 由 mime 派生对象扩展名（白名单）；非白名单统一 .bin
const char* mimeExt(const std::string& mime) {
  if (mime == "image/png") return ".png";
  if (mime == "image/jpeg") return ".jpg";
  if (mime == "image/gif") return ".gif";
  if (mime == "image/webp") return ".webp";
  return ".bin";
}

// 当前 UTC 年/月（用于对象键分目录）
void utcYearMonth(int* year, int* month) {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  *year = tm.tm_year + 1900;
  *month = tm.tm_mon + 1;
}

std::string intToHex2(unsigned v) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.push_back(kHex[(v >> 4) & 0xF]);
  out.push_back(kHex[v & 0xF]);
  return out;
}

}  // namespace

MediaService::MediaService(std::shared_ptr<MediaRepository> repo,
                           std::shared_ptr<ObjectStore> store,
                           std::shared_ptr<RateLimiterStore> limiter,
                           std::string bucketPathPrefix)
    : repo_(std::move(repo)),
      store_(std::move(store)),
      limiter_(std::move(limiter)),
      prefix_(std::move(bucketPathPrefix)) {}

bool MediaService::validMime(const std::string& mime) {
  return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" ||
         mime == "image/webp";
}

bool MediaService::validKind(const std::string& kind) {
  // V1 仅支持图片；文件/语音/视频留待 V2/V3
  return kind == "image";
}

std::string MediaService::randomFileId() {
  // CSPRNG 取 16 字节 → 32 hex（与 user_store token 随机源一致，不使用 rand()）
  std::random_device rd;
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 16; ++i) {
    out += intToHex2(static_cast<unsigned>(rd()) & 0xFFu);
  }
  return out;
}

std::string MediaService::objectKey(int64_t userId, const std::string& fileId,
                                    const std::string& mime) {
  int year = 0;
  int month = 0;
  utcYearMonth(&year, &month);
  char ym[32];  // 容 int 极值下的 "%04d/%02d" 最坏展开（防 -Wformat-truncation）
  std::snprintf(ym, sizeof(ym), "%04d/%02d", year, month);
  return prefix_ + "/" + std::to_string(userId) + "/" + ym + "/" + fileId +
         mimeExt(mime);
}

PresignResult MediaService::presign(int64_t userId, const std::string& fileName,
                                    const std::string& mimeType, int64_t size,
                                    const std::string& kind) {
  PresignResult r;
  if (!validKind(kind)) {
    r.code = 400;
    r.msg = "不支持的媒体类型";
    return r;
  }
  if (fileName.empty() || fileName.size() > config::kMediaFileNameMaxLen) {
    r.code = 400;
    r.msg = "文件名非法";
    return r;
  }
  if (!validMime(mimeType)) {
    r.code = 400;
    r.msg = "不支持的媒体格式";
    return r;
  }
  if (size <= 0) {
    r.code = 400;
    r.msg = "文件大小非法";
    return r;
  }
  if (size > static_cast<int64_t>(config::kMediaSizeMaxBytes)) {
    r.code = 413;
    r.msg = "文件过大";
    return r;
  }
  if (!store_ || !store_->enabled()) {
    r.code = 503;
    r.msg = "对象存储不可用";
    return r;
  }
  // 限流器缺失时与 store_ 一致 fail-closed，避免无限流绕过
  if (!limiter_) {
    r.code = 503;
    r.msg = "限流服务不可用";
    return r;
  }
  if (!limiter_->allow(userId, config::kMediaPresignPerWindow,
                       config::kMediaWindow)) {
    r.code = 429;
    r.msg = "请求过于频繁，请稍后再试";
    return r;
  }

  MediaRecord m;
  m.fileId = randomFileId();
  m.uploaderId = userId;
  m.objectKey = objectKey(userId, m.fileId, mimeType);
  m.fileName = fileName;
  m.mimeType = mimeType;
  m.fileSize = size;
  m.status = kMediaPending;
  m.createdAt = static_cast<int64_t>(std::time(nullptr));
  if (!repo_ || !repo_->insertPending(m)) {
    r.code = 500;
    r.msg = "服务内部错误";
    return r;
  }

  PresignedUrl put = store_->presignPut(m.objectKey, config::kMediaPutTtlSec);
  if (put.url.empty()) {
    // 补偿：签名失败则回收已落的 PENDING 行，避免遗留孤儿记录
    repo_->markDeleted(m.fileId);
    r.code = 500;
    r.msg = "签名失败";
    return r;
  }
  r.ok = true;
  r.code = 0;
  r.msg = "ok";
  r.fileId = m.fileId;
  r.uploadUrl = put.url;
  r.expiresIn = put.expiresIn;
  return r;
}

CommitResult MediaService::commit(int64_t userId, const std::string& fileId,
                                  int64_t size, int width, int height,
                                  int duration) {
  CommitResult r;
  if (fileId.empty()) {
    r.code = 400;
    r.msg = "参数错误";
    return r;
  }
  if (!repo_) {
    r.code = 500;
    r.msg = "服务内部错误";
    return r;
  }
  auto rec = repo_->findByFileId(fileId);
  if (!rec) {
    r.code = 404;
    r.msg = "媒体不存在";
    return r;
  }
  if (rec->uploaderId != userId) {
    r.code = 403;
    r.msg = "无权操作该媒体";
    return r;
  }
  // 幂等：已 COMMITTED 直接成功，不重复校验
  if (rec->status == kMediaCommitted) {
    r.ok = true;
    r.code = 0;
    r.msg = "ok";
    return r;
  }
  if (rec->status != kMediaPending) {
    r.code = 400;
    r.msg = "媒体状态不可提交";
    return r;
  }
  if (!store_ || !store_->enabled()) {
    r.code = 503;
    r.msg = "对象存储不可用";
    return r;
  }
  ObjectInfo info = store_->headObject(rec->objectKey);
  if (!info.exists) {
    r.code = 400;
    r.msg = "对象不存在，请先上传";
    return r;
  }
  // 以 HEAD 实际大小为准复核上限，防止「声明小 size 拿签名 → 传超大对象」绕过
  if (info.size == 0) {
    r.code = 400;
    r.msg = "空文件";
    return r;
  }
  if (info.size > static_cast<uint64_t>(config::kMediaSizeMaxBytes)) {
    r.code = 413;
    r.msg = "文件过大";
    return r;
  }
  // 客户端声明大小须与 HEAD 实际一致；入库以 HEAD 为准
  if (size != static_cast<int64_t>(info.size)) {
    r.code = 400;
    r.msg = "文件大小不匹配";
    return r;
  }
  // 内容校验（P0-3）：下载对象前缀，按 magic 复核真实类型与维度。
  // 声明 MIME 只是白名单提示，实际字节必须匹配（防伪装文件/SVG/超限图进入 COMMITTED）。
  std::vector<uint8_t> prefix;
  if (!store_->getObject(rec->objectKey, config::kMediaValidatePrefixBytes, &prefix)) {
    // HEAD 成功但 GET 失败：对象存储异常，503 可重试（不计校验拒绝）
    r.code = 503;
    r.msg = "对象存储不可用";
    return r;
  }
  int vw = 0, vh = 0;
  const MediaKind kind = MediaStore::detectKind(prefix, &vw, &vh);
  if (kind == MediaKind::kReject || rec->mimeType != MediaStore::mimeOf(kind)) {
    // 非法内容或声明 MIME 与实际不符：保持 PENDING（由 PENDING GC 兜底回收）
    g_metrics.mediaCommitRejected.fetch_add(1, std::memory_order_relaxed);
    LOG_W("[media] commit 校验拒绝：声明 mime=" << rec->mimeType
          << " 实际=" << MediaStore::mimeOf(kind) << " bytes(前缀)=" << prefix.size());
    r.code = 400;
    r.msg = "媒体内容校验失败";
    return r;
  }
  // 宽高以服务端解析值为准（客户端上报值忽略），与「size 以 HEAD 为准」同一先例
  (void)width;
  (void)height;
  if (!repo_->commit(fileId, static_cast<int64_t>(info.size), vw, vh, duration)) {
    r.code = 500;
    r.msg = "提交失败";
    return r;
  }
  r.ok = true;
  r.code = 0;
  r.msg = "ok";
  return r;
}

UrlResult MediaService::url(int64_t userId, const std::string& fileId,
                            const MediaAccessFn& canAccess) {
  UrlResult r;
  if (fileId.empty()) {
    r.code = 400;
    r.msg = "参数错误";
    return r;
  }
  if (!canAccess || !canAccess(userId, fileId)) {
    r.code = 403;
    r.msg = "无权访问该媒体";
    return r;
  }
  if (!store_ || !store_->enabled()) {
    r.code = 503;
    r.msg = "对象存储不可用";
    return r;
  }
  auto rec = repo_ ? repo_->findByFileId(fileId) : std::optional<MediaRecord>();
  if (!rec) {
    r.code = 404;
    r.msg = "媒体不存在";
    return r;
  }
  // 仅已 COMMITTED 的媒体可访问；PENDING/GC 记录不签发 GET URL
  if (rec->status != kMediaCommitted) {
    r.code = 404;
    r.msg = "媒体不存在";
    return r;
  }
  PresignedUrl get = store_->presignGet(rec->objectKey, config::kMediaGetTtlSec);
  if (get.url.empty()) {
    r.code = 500;
    r.msg = "签名失败";
    return r;
  }
  r.ok = true;
  r.code = 0;
  r.msg = "ok";
  r.url = get.url;
  r.expiresIn = get.expiresIn;
  return r;
}

GcStats MediaService::gcOnce(int64_t nowSec) {
  GcStats stats;
  if (!repo_) return stats;
  // 对象存储不可用（未装配或未启用）时无法确认对象删除：整轮跳过（保留状态，
  // 待存储恢复后下轮重试）。抢占后无法删除会把记录永久标成已回收（P1-2 审计缺陷）。
  if (!store_ || !store_->enabled()) return stats;
  const int64_t now = nowSec > 0 ? nowSec : static_cast<int64_t>(std::time(nullptr));

  // ---- 1) 超期 PENDING：CAS 0→3 抢占，删对象失败回滚 3→0（下轮重试）----
  const int64_t pendingUpper = now - config::kMediaPendingTtlSec;
  for (const auto& m : repo_->findPendingOlderThan(pendingUpper, config::kMediaGcBatch)) {
    if (!repo_->markDeleted(m.fileId)) continue;  // 已被 commit/其他轮处理
    if (!m.objectKey.empty() && !store_->deleteObject(m.objectKey)) {
      // 删除失败：回滚 PENDING，下轮重试（不再"标记即丢弃"）
      if (!repo_->revertGc(m.fileId, kMediaPending)) {
        LOG_W("[media] GC 回滚失败 file_id=" << m.fileId);
      }
      ++stats.deleteFailures;
      LOG_W("[media] GC 删除对象失败（已回滚待重试）: " << m.objectKey);
      continue;
    }
    ++stats.pendingReclaimed;
  }

  // ---- 2) 无引用超期 COMMITTED（孤儿）：CAS 2→3 抢占，抢占后复查引用，删失败回滚 3→2 ----
  const int64_t orphanUpper = now - config::kMediaOrphanTtlSec;
  for (const auto& m : repo_->findCommittedOrphanOlderThan(orphanUpper,
                                                           config::kMediaGcBatch)) {
    if (!repo_->markOrphanGc(m.fileId)) continue;  // 已非 COMMITTED（并发发送/其它轮）
    // 抢占后复查：查询列表与抢占之间可能有并发 msg.send 建立引用
    const int ref = repo_->isReferenced(m.fileId);
    if (ref != 0) {  // 1=被引用；-1=查询失败（未知，不可当未引用）——均回滚
      if (!repo_->revertGc(m.fileId, kMediaCommitted)) {
        LOG_W("[media] GC 孤儿回滚失败 file_id=" << m.fileId);
      }
      continue;
    }
    if (!m.objectKey.empty() && !store_->deleteObject(m.objectKey)) {
      if (!repo_->revertGc(m.fileId, kMediaCommitted)) {
        LOG_W("[media] GC 孤儿回滚失败 file_id=" << m.fileId);
      }
      ++stats.deleteFailures;
      LOG_W("[media] GC 删除孤儿对象失败（已回滚待重试）: " << m.objectKey);
      continue;
    }
    ++stats.orphanReclaimed;
  }
  return stats;
}

}  // namespace im
