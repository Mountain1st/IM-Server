// ============================================================
// media_store：图片解码 + 类型维度校验 + HMAC 对象键原子落盘
// - base64 字符集检查 + 解码（拒绝非字母表输入）
// - magic + 维度解析：PNG IHDR / JPEG SOF0-3 / GIF 逻辑屏 / WebP VP8/VP8L
// - SVG（含 `<svg`，大小写不敏感）直接拒绝；任一边 >4096 拒绝
// - HMAC-SHA256(内容, 服务端密钥) 全长 hex + 扩展名作为对象键
// - tmp + fsync + rename 原子落盘；同内容goto去重（目标已存在直接复用）
// - 日志只记大小 / mime / 文件名，不打印图片字节
// ============================================================
#include "media_store.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "log.hpp"
#include "util.hpp"

namespace im {
namespace {

// HMAC 密钥来源：环境变量 IM_MEDIA_HMAC_KEY；
// 未配置时用固定默认值（单实例语义：单机部署内容寻址去重可用，
// 多实例/重建密钥后旧 URL 即 404；与项目现有 env 覆盖风格一致）。
std::string mediaHmacKey() {
  if (const char* v = std::getenv("IM_MEDIA_HMAC_KEY")) {
    if (v[0] != '\0') return std::string(v);
  }
  return std::string("im-default-single-instance-media-key");
}

// 二进制转全长小写 hex
std::string toHex(const std::string& raw) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0F]);
  }
  return out;
}

// 大端读 16/32 位
uint32_t readBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t readBe16(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

// 维度合法性：正数且任一边不超过上限
bool dimOk(int w, int h) {
  return w > 0 && h > 0 && w <= MediaStore::kMaxDimensionPx && h <= MediaStore::kMaxDimensionPx;
}

// 载荷中是否含 `<svg`（大小写不敏感，全量扫描，禁日志打字节故只做内存比较）
bool containsSvgTag(const std::vector<uint8_t>& bytes) {
  static const char kNeedle[] = "<svg";
  if (bytes.size() < 4) return false;
  for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
    if (bytes[i] != '<') continue;
    bool hit = true;
    for (int k = 0; k < 4; ++k) {
      char c = static_cast<char>(std::tolower(bytes[i + static_cast<size_t>(k)]));
      if (c != kNeedle[k]) {
        hit = false;
        break;
      }
    }
    if (hit) return true;
  }
  return false;
}

const char* kindExt(MediaKind k) {
  switch (k) {
    case MediaKind::kPng: return "png";
    case MediaKind::kJpeg: return "jpg";
    case MediaKind::kGif: return "gif";
    case MediaKind::kWebp: return "webp";
    case MediaKind::kReject: return "bin";
  }
  return "bin";
}

}  // namespace

const char* MediaStore::mimeOf(MediaKind k) {
  switch (k) {
    case MediaKind::kPng: return "image/png";
    case MediaKind::kJpeg: return "image/jpeg";
    case MediaKind::kGif: return "image/gif";
    case MediaKind::kWebp: return "image/webp";
    case MediaKind::kReject: return "application/octet-stream";
  }
  return "application/octet-stream";
}

// 对象键命名模式检查：64 位小写 hex + 图片扩展名
bool isMediaObjectName(const std::string& name) {
  const size_t dot = name.rfind('.');
  if (dot != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    const char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  const std::string ext = name.substr(65);
  return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "webp";
}

size_t MediaStore::gcExpired(const std::string& dir, int64_t olderThanSec) {
  size_t removed = 0;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec) || ec) return 0;
  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) return 0;
  for (const auto& entry : it) {
    // 符号链接一律跳过（只看链接本身不跟随目标，防删出界）
    std::error_code sec;
    if (entry.is_symlink(sec) || sec) continue;
    if (!entry.is_regular_file(sec) || sec) continue;
    const std::string name = entry.path().filename().string();
    if (!isMediaObjectName(name)) continue;
    std::error_code tec;
    const auto mtime = std::filesystem::last_write_time(entry.path(), tec);
    if (tec) continue;
    // 同一 file_clock 下求年龄（秒），避免跨时钟换算
    const int64_t ageSec =
        std::chrono::duration_cast<std::chrono::seconds>(now - mtime).count();
    if (ageSec <= olderThanSec) continue;
    std::error_code rec;
    if (std::filesystem::remove(entry.path(), rec) && !rec) ++removed;
  }
  // 只记数量与目录，不打文件名（防日志膨胀）
  if (removed > 0) LOG_I("[media] 过期清理删除 " << removed << " 个文件：dir=" << dir);
  return removed;
}

bool MediaStore::isValidBase64(std::string_view s) {
  if (s.empty() || s.size() % 4 != 0) return false;
  size_t pad = 0;
  if (s.size() >= 1 && s.back() == '=') ++pad;
  if (s.size() >= 2 && s[s.size() - 2] == '=') ++pad;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '/' || c == '=';
    if (!ok) return false;
    if (c == '=' && i < s.size() - 2) return false;  // 填充只允许尾部最多 2 个
  }
  if (pad > 2) return false;
  return true;
}

std::optional<std::vector<uint8_t>> MediaStore::b64Decode(std::string_view s) {
  if (!isValidBase64(s)) return std::nullopt;
  static const std::array<int, 256> kRev = [] {
    std::array<int, 256> t{};
    t.fill(-1);
    for (int i = 0; i < 26; ++i) t[static_cast<size_t>('A') + static_cast<size_t>(i)] = i;
    for (int i = 0; i < 26; ++i) t[static_cast<size_t>('a') + static_cast<size_t>(i)] = 26 + i;
    for (int i = 0; i < 10; ++i) t[static_cast<size_t>('0') + static_cast<size_t>(i)] = 52 + i;
    t[static_cast<size_t>('+')] = 62;
    t[static_cast<size_t>('/')] = 63;
    t[static_cast<size_t>('=')] = 0;
    return t;
  }();
  std::vector<uint8_t> out;
  out.reserve(s.size() / 4 * 3);
  for (size_t i = 0; i < s.size(); i += 4) {
    int v0 = kRev[static_cast<unsigned char>(s[i])];
    int v1 = kRev[static_cast<unsigned char>(s[i + 1])];
    int v2 = kRev[static_cast<unsigned char>(s[i + 2])];
    int v3 = kRev[static_cast<unsigned char>(s[i + 3])];
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return std::nullopt;
    uint32_t n = (uint32_t(v0) << 18) | (uint32_t(v1) << 12) | (uint32_t(v2) << 6) | uint32_t(v3);
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    if (s[i + 2] != '=') out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    if (s[i + 3] != '=') out.push_back(static_cast<uint8_t>(n & 0xFF));
  }
  return out;
}

MediaKind MediaStore::detectKind(const std::vector<uint8_t>& bytes, int* w, int* h) {
  auto setWh = [&](int ww, int hh) {
    if (w != nullptr) *w = ww;
    if (h != nullptr) *h = hh;
  };
  // SVG 文本（含 `<svg`）直接拒绝，优先于 magic 判断
  if (containsSvgTag(bytes)) {
    setWh(0, 0);
    return MediaKind::kReject;
  }
  const size_t n = bytes.size();
  const uint8_t* p = n > 0 ? bytes.data() : nullptr;

  // ---- PNG：签名 + IHDR 宽高 ----
  static const uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (n >= 33 && std::memcmp(p, kPngSig, 8) == 0) {
    uint32_t len = readBe32(p + 8);
    if (std::memcmp(p + 12, "IHDR", 4) == 0 && len >= 13 && n >= 8 + 8 + 13) {
      int ww = static_cast<int>(readBe32(p + 16));
      int hh = static_cast<int>(readBe32(p + 20));
      setWh(ww, hh);
      if (!dimOk(ww, hh)) return MediaKind::kReject;
      return MediaKind::kPng;
    }
    setWh(0, 0);
    return MediaKind::kReject;
  }

  // ---- GIF：GIF87a/GIF89a + 逻辑屏幕宽高（LE16） ----
  if (n >= 10 && (std::memcmp(p, "GIF87a", 6) == 0 || std::memcmp(p, "GIF89a", 6) == 0)) {
    int ww = int(p[6]) | (int(p[7]) << 8);
    int hh = int(p[8]) | (int(p[9]) << 8);
    setWh(ww, hh);
    if (!dimOk(ww, hh)) return MediaKind::kReject;
    return MediaKind::kGif;
  }

  // ---- JPEG：SOI 起，扫描 SOF0-SOF3 取维度 ----
  if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
    size_t i = 2;
    while (i + 4 <= n) {
      if (p[i] != 0xFF) break;
      // 跳过填充 FF
      while (i < n && p[i] == 0xFF) ++i;
      if (i >= n) break;
      uint8_t marker = p[i++];
      if (marker == 0xD9) break;  // EOI：未找到 SOF
      if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) continue;  // 无长度段
      if (i + 2 > n) break;
      uint16_t segLen = readBe16(p + i);
      if (segLen < 2) break;
      if (marker >= 0xC0 && marker <= 0xC3) {  // SOF0-SOF3
        if (i + 7 > n) break;
        int hh = int(readBe16(p + i + 3));
        int ww = int(readBe16(p + i + 5));
        setWh(ww, hh);
        if (!dimOk(ww, hh)) return MediaKind::kReject;
        return MediaKind::kJpeg;
      }
      if (marker == 0xDA) break;  // SOS 之后为熵编码数据，不再有 SOF
      i += segLen;  // segLen 含自身 2 字节；此处 i 指向长度字段，+segLen 跳到下一标记的 0xFF
      // 上式：i 指向 len 高字节，段总占 segLen+? 修正：标记(已消费)+len(2)+payload(segLen-2)，
      // i 当前在 len 处，下一标记在 i+segLen 处
    }
    setWh(0, 0);
    return MediaKind::kReject;
  }

  // ---- WebP：RIFF....WEBP + VP8/VP8L ----
  if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) {
    if (n >= 30 && std::memcmp(p + 12, "VP8 ", 4) == 0) {
      // 有损：chunk 数据起 20，帧 tag 3 字节 + 起始码 9D 01 2A
      if (p[23] == 0x9D && p[24] == 0x01 && p[25] == 0x2A) {
        int ww = (int(p[26]) | (int(p[27]) << 8)) & 0x3FFF;
        int hh = (int(p[28]) | (int(p[29]) << 8)) & 0x3FFF;
        setWh(ww, hh);
        if (!dimOk(ww, hh)) return MediaKind::kReject;
        return MediaKind::kWebp;
      }
      setWh(0, 0);
      return MediaKind::kReject;
    }
    if (n >= 25 && std::memcmp(p + 12, "VP8L", 4) == 0) {
      // 无损：20 为签名 0x2F，后 4 字节打包宽高（各减 1 存储）
      if (p[20] == 0x2F) {
        uint32_t b0 = p[21];
        uint32_t b1 = p[22];
        uint32_t b2 = p[23];
        uint32_t b3 = p[24];
        int ww = static_cast<int>((b0 | ((b1 & 0x3F) << 8)) + 1);
        int hh = static_cast<int>((((b1 >> 6) & 0x03)) | (b2 << 2) | ((b3 & 0x0F) << 10)) + 1;
        setWh(ww, hh);
        if (!dimOk(ww, hh)) return MediaKind::kReject;
        return MediaKind::kWebp;
      }
      setWh(0, 0);
      return MediaKind::kReject;
    }
    setWh(0, 0);
    return MediaKind::kReject;
  }

  setWh(0, 0);
  return MediaKind::kReject;
}

std::optional<std::string> MediaStore::decodeAndStore(std::string_view b64, const std::string& dir) {
  if (!isValidBase64(b64)) {
    LOG_W("[media] 非法 base64：len=" << b64.size());
    return std::nullopt;
  }
  auto raw = b64Decode(b64);
  if (!raw) {
    LOG_W("[media] base64 解码失败：len=" << b64.size());
    return std::nullopt;
  }
  int w = 0;
  int h = 0;
  MediaKind kind = detectKind(*raw, &w, &h);
  if (kind == MediaKind::kReject) {
    // 只记大小，不打字节
    LOG_W("[media] 图片校验拒绝：bytes=" << raw->size());
    return std::nullopt;
  }

  // HMAC-SHA256(内容) 全长 hex + 扩展名
  std::string content(reinterpret_cast<const char*>(raw->data()), raw->size());
  std::string hex = toHex(hmacSha256Raw(mediaHmacKey(), content));
  const char* ext = kindExt(kind);
  std::string fileName = hex + "." + ext;

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    LOG_E("[media] 建目录失败：dir=" << dir);
    return std::nullopt;
  }
  std::string target = dir + "/" + fileName;
  if (std::filesystem::exists(target, ec) && !ec) {
    LOG_I("[media] 命中去重：mime=" << MediaStore::mimeOf(kind) << " bytes=" << raw->size() << " file=" << fileName);
    return std::string("/uploads/") + fileName;
  }

  // tmp + fsync + rename 原子落盘
  std::string tmp = target + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    LOG_E("[media] 建临时文件失败：bytes=" << raw->size());
    return std::nullopt;
  }
  size_t off = 0;
  bool writeOk = true;
  while (off < raw->size()) {
    ssize_t nw = ::write(fd, (*raw).data() + off, raw->size() - off);
    if (nw <= 0) {
      writeOk = false;
      break;
    }
    off += static_cast<size_t>(nw);
  }
  if (writeOk && ::fsync(fd) != 0) writeOk = false;
  ::close(fd);
  if (!writeOk) {
    std::remove(tmp.c_str());
    LOG_E("[media] 写临时文件失败：bytes=" << raw->size());
    return std::nullopt;
  }
  if (std::rename(tmp.c_str(), target.c_str()) != 0) {
    std::remove(tmp.c_str());
    LOG_E("[media] 落盘更名失败：bytes=" << raw->size());
    return std::nullopt;
  }
  // 只记大小 / mime / 文件名，不打字节
  LOG_I("[media] 落盘成功：mime=" << MediaStore::mimeOf(kind) << " bytes=" << raw->size() << " w=" << w << " h=" << h
                                 << " file=" << fileName);
  return std::string("/uploads/") + fileName;
}

std::string MediaStore::b64EncodeForTest(const std::string& s) {
  static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((s.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < s.size()) {
    uint32_t n = (uint32_t(uint8_t(s[i])) << 16) | (uint32_t(uint8_t(s[i + 1])) << 8) |
                 uint32_t(uint8_t(s[i + 2]));
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += kTable[n & 0x3F];
    i += 3;
  }
  size_t rem = s.size() - i;
  if (rem == 1) {
    uint32_t n = uint32_t(uint8_t(s[i])) << 16;
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += "==";
  } else if (rem == 2) {
    uint32_t n =
        (uint32_t(uint8_t(s[i])) << 16) | (uint32_t(uint8_t(s[i + 1])) << 8);
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

}  // namespace im
