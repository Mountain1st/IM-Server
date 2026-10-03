#pragma once

#include "media_repository.hpp"
#include "object_store.hpp"
#include "rate_limiter_store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace im {

// 预签名结果：ok=false 时 code/msg 描述失败原因
struct PresignResult {
  bool ok = false;
  int code = 0;
  std::string msg;
  std::string fileId;
  std::string uploadUrl;
  int64_t expiresIn = 0;
};

// 提交结果：仅成功/失败与错误码
struct CommitResult { bool ok = false; int code = 0; std::string msg; };

// 取 URL 结果：成功时带短期 GET 预签名 URL
struct UrlResult {
  bool ok = false;
  int code = 0;
  std::string msg;
  std::string url;
  int64_t expiresIn = 0;
};

// GC 单轮结果（任务② M2）：删除失败不计入回收数（状态已回滚待下轮重试）
struct GcStats {
  int64_t pendingReclaimed = 0;  // 超期 PENDING 成功回收数
  int64_t orphanReclaimed = 0;   // 无引用 COMMITTED 成功回收数
  int64_t deleteFailures = 0;    // 对象删除失败数（已回滚）
};

// 权限校验回调：由 AppServer 注入，判断 user 是否有权访问引用 fileId 的消息
using MediaAccessFn = std::function<bool(int64_t userId, const std::string& fileId)>;

// 媒体业务编排：presign（签名+落 PENDING）/commit（HEAD 校验）/url（鉴权+签名）
// 以及超期 PENDING 回收。无状态，依赖注入的仓储/对象存储/限流器。
class MediaService {
 public:
  MediaService(std::shared_ptr<MediaRepository> repo,
               std::shared_ptr<ObjectStore> store,
               std::shared_ptr<RateLimiterStore> limiter,
               std::string bucketPathPrefix = "media");

  PresignResult presign(int64_t userId, const std::string& fileName,
                        const std::string& mimeType, int64_t size,
                        const std::string& kind);
  CommitResult commit(int64_t userId, const std::string& fileId, int64_t size,
                      int width, int height, int duration);
  UrlResult url(int64_t userId, const std::string& fileId,
                const MediaAccessFn& canAccess);
  // 清理一轮：超期 PENDING + 无引用超期 COMMITTED（孤儿）。删对象失败回滚状态，
  // 下轮重试，不计入回收数。nowSec 供测试注入（0=取当前时间）
  GcStats gcOnce(int64_t nowSec = 0);

  static bool validMime(const std::string& mime);
  static bool validKind(const std::string& kind);

 private:
  std::string objectKey(int64_t userId, const std::string& fileId,
                        const std::string& mime);
  static std::string randomFileId();  // 32 hex（复用 util 随机字节）

  std::shared_ptr<MediaRepository> repo_;
  std::shared_ptr<ObjectStore> store_;
  std::shared_ptr<RateLimiterStore> limiter_;
  std::string prefix_;
};

}  // namespace im
