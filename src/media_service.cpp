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

#include "server_config.hpp"

#include <cstdio>
#include <ctime>
#include <random>
#include <utility>

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
  char ym[16];
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
  if (info.size == 0 ||
      info.size > static_cast<uint64_t>(config::kMediaSizeMaxBytes)) {
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
  if (!repo_->commit(fileId, static_cast<int64_t>(info.size), width, height,
                     duration)) {
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

int64_t MediaService::gcOnce() {
  if (!repo_) return 0;
  const int64_t upperBound =
      static_cast<int64_t>(std::time(nullptr)) - config::kMediaPendingTtlSec;
  auto list = repo_->findPendingOlderThan(upperBound, config::kMediaGcBatch);
  int64_t removed = 0;
  for (const auto& m : list) {
    // 对象存储不可用（未装配或未启用）时无法确认对象删除：
    // 若此时仍 markDeleted，会跳过删对象而留下永久孤儿对象。
    // 故直接跳过本行，保留 PENDING，待对象存储恢复后下轮重试（可能本轮无进展）。
    if (!store_ || !store_->enabled()) {
      continue;
    }
    // 先删对象（对象可能从未上传，deleteObject 幂等）
    if (!m.objectKey.empty() && !store_->deleteObject(m.objectKey)) {
      // 删除失败：保持 PENDING，待下轮重试，避免留下永久孤儿对象
      continue;
    }
    repo_->markDeleted(m.fileId);
    ++removed;
  }
  return removed;
}

}  // namespace im
