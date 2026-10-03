// tests/media_service_test.cpp —— MediaService 纯逻辑（fake 仓储/对象存储，不连真机）
// M1：commit 内容校验（magic/MIME 一致性/维度上限/前缀读取失败）
#include "test_util.hpp"

#include "media_service.hpp"
#include "media_store.hpp"
#include "rate_limiter_store.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace im;

namespace {

// 构造 PNG 头：签名 + IHDR（指定宽高）
std::vector<uint8_t> pngHeader(uint32_t w, uint32_t h) {
  std::vector<uint8_t> b = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
                            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  b[16] = static_cast<uint8_t>((w >> 24) & 0xFF);
  b[17] = static_cast<uint8_t>((w >> 16) & 0xFF);
  b[18] = static_cast<uint8_t>((w >> 8) & 0xFF);
  b[19] = static_cast<uint8_t>(w & 0xFF);
  b[20] = static_cast<uint8_t>((h >> 24) & 0xFF);
  b[21] = static_cast<uint8_t>((h >> 16) & 0xFF);
  b[22] = static_cast<uint8_t>((h >> 8) & 0xFF);
  b[23] = static_cast<uint8_t>(h & 0xFF);
  return b;
}

std::vector<uint8_t> gifHeader(uint16_t w, uint16_t h) {
  std::vector<uint8_t> b = {'G', 'I', 'F', '8', '9', 'a',
                            static_cast<uint8_t>(w & 0xFF), static_cast<uint8_t>((w >> 8) & 0xFF),
                            static_cast<uint8_t>(h & 0xFF), static_cast<uint8_t>((h >> 8) & 0xFF)};
  return b;
}

class FakeStore : public ObjectStore {
 public:
  bool enabled() const override { return true; }
  PresignedUrl presignPut(const std::string&, int64_t, const std::string&) override {
    return PresignedUrl{"http://fake/put", 900};
  }
  PresignedUrl presignGet(const std::string&, int64_t, const std::string&) override {
    return PresignedUrl{"http://fake/get", 3600};
  }
  ObjectInfo headObject(const std::string& key) override {
    ObjectInfo i;
    auto it = objects.find(key);
    if (it != objects.end()) {
      i.exists = true;
      i.size = it->second.size();
    }
    return i;
  }
  bool deleteObject(const std::string& key) override {
    if (failDelete) return false;
    return objects.erase(key) > 0;
  }
  bool getObject(const std::string& key, size_t maxBytes,
                 std::vector<uint8_t>* out) override {
    if (failGet) return false;
    auto it = objects.find(key);
    if (it == objects.end()) return false;
    const size_t n = std::min(maxBytes, it->second.size());
    out->assign(it->second.begin(), it->second.begin() + static_cast<long>(n));
    return true;
  }

  std::map<std::string, std::vector<uint8_t>> objects;
  bool failDelete = false;
  bool failGet = false;
};

class FakeRepo : public MediaRepository {
 public:
  bool ok() const override { return true; }
  bool insertPending(const MediaRecord& m) override {
    recs[m.fileId] = m;
    return true;
  }
  std::optional<MediaRecord> findByFileId(const std::string& fid) override {
    auto it = recs.find(fid);
    if (it == recs.end()) return std::nullopt;
    return it->second;
  }
  bool commit(const std::string& fid, int64_t size, int w, int h, int d) override {
    auto it = recs.find(fid);
    if (it == recs.end()) return false;
    if (it->second.status == kMediaCommitted) return true;
    if (it->second.status != kMediaPending) return false;
    it->second.status = kMediaCommitted;
    it->second.fileSize = size;
    it->second.width = w;
    it->second.height = h;
    it->second.duration = d;
    return true;
  }
  bool markDeleted(const std::string& fid) override {
    auto it = recs.find(fid);
    if (it == recs.end() || it->second.status != kMediaPending) return false;
    it->second.status = kMediaGc;
    return true;
  }
  std::vector<MediaRecord> findPendingOlderThan(int64_t upper, int limit) override {
    std::vector<MediaRecord> out;
    for (const auto& [_, m] : recs) {
      if (m.status == kMediaPending && m.createdAt < upper &&
          static_cast<int>(out.size()) < limit) {
        out.push_back(m);
      }
    }
    return out;
  }
  int isReferenced(const std::string& fid) override {
    if (refFail) return -1;
    return referenced.count(fid) ? 1 : 0;
  }

  std::map<std::string, MediaRecord> recs;
  std::map<std::string, bool> referenced;
  bool refFail = false;
};

struct Ctx {
  std::shared_ptr<FakeRepo> repo = std::make_shared<FakeRepo>();
  std::shared_ptr<FakeStore> store = std::make_shared<FakeStore>();
  std::shared_ptr<RateLimiterStore> limiter = std::make_shared<MemoryRateLimiter>();
  std::unique_ptr<MediaService> svc;
  Ctx() { svc = std::make_unique<MediaService>(repo, store, limiter); }
};

// 落一条 PENDING（含对象字节）
std::string seedPending(Ctx& c, const std::string& mime,
                        const std::vector<uint8_t>& bytes) {
  MediaRecord m;
  m.fileId = "0123456789abcdef0123456789abcdef";
  m.uploaderId = 7;
  m.objectKey = "media/7/" + m.fileId + ".bin";
  m.fileName = "x";
  m.mimeType = mime;
  m.fileSize = static_cast<int64_t>(bytes.size());
  m.status = kMediaPending;
  m.createdAt = 1000;
  c.repo->insertPending(m);
  c.store->objects[m.objectKey] = bytes;
  return m.fileId;
}

}  // namespace

TEST(commit_real_png_passes_and_stores_parsed_dims) {
  Ctx c;
  const auto fid = seedPending(c, "image/png", pngHeader(2, 3));
  const auto r = c.svc->commit(7, fid, 33, 999, 999, 0);
  CHECK(r.ok);
  CHECK(r.code == 0);
  auto rec = c.repo->findByFileId(fid);
  CHECK(rec && rec->status == kMediaCommitted);
  CHECK(rec && rec->width == 2 && rec->height == 3);  // 以服务端解析为准
}

TEST(commit_non_png_bytes_rejected_keeps_pending) {
  Ctx c;
  const auto fid = seedPending(c, "image/png", {'n', 'o', 't', 'a', 'p', 'n', 'g'});
  const auto r = c.svc->commit(7, fid, 7, 0, 0, 0);
  CHECK(!r.ok);
  CHECK(r.code == 400);
  auto rec = c.repo->findByFileId(fid);
  CHECK(rec && rec->status == kMediaPending);  // 保持 PENDING 由 GC 兜底
}

TEST(commit_mime_mismatch_rejected) {
  // 真 PNG 声明为 JPEG：白名单通过但实际 magic 不符 → 拒绝
  Ctx c;
  const auto fid = seedPending(c, "image/jpeg", pngHeader(2, 2));
  const auto r = c.svc->commit(7, fid, 33, 0, 0, 0);
  CHECK(!r.ok);
  CHECK(r.code == 400);
}

TEST(commit_oversize_dimension_rejected) {
  Ctx c;
  const auto fid = seedPending(c, "image/png", pngHeader(5000, 1));
  const auto r = c.svc->commit(7, fid, 33, 0, 0, 0);
  CHECK(!r.ok);
  CHECK(r.code == 400);
}

TEST(commit_prefix_fetch_failure_is_503_not_reject) {
  Ctx c;
  const auto fid = seedPending(c, "image/png", pngHeader(2, 2));
  c.store->failGet = true;
  const auto r = c.svc->commit(7, fid, 33, 0, 0, 0);
  CHECK(!r.ok);
  CHECK(r.code == 503);  // 存储异常可重试，不计内容拒绝
  auto rec = c.repo->findByFileId(fid);
  CHECK(rec && rec->status == kMediaPending);
}

TEST(commit_gif_mime_accepted) {
  Ctx c;
  const auto fid = seedPending(c, "image/gif", gifHeader(4, 5));
  const auto r = c.svc->commit(7, fid, 10, 0, 0, 0);
  CHECK(r.ok);
  auto rec = c.repo->findByFileId(fid);
  CHECK(rec && rec->width == 4 && rec->height == 5);
}

int main() {
  RUN(commit_real_png_passes_and_stores_parsed_dims);
  RUN(commit_non_png_bytes_rejected_keeps_pending);
  RUN(commit_mime_mismatch_rejected);
  RUN(commit_oversize_dimension_rejected);
  RUN(commit_prefix_fetch_failure_is_503_not_reject);
  RUN(commit_gif_mime_accepted);
  std::printf("media_service_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
