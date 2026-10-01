#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace im {

// 图片类型：未知/非法统一为 kReject
enum class MediaKind { kPng, kJpeg, kGif, kWebp, kReject };

// 图片解码 + 维度校验 + 对象键落盘（无状态静态工具类）
class MediaStore {
 public:
  // 图片宽高上限（任一边超出即拒绝）
  static constexpr int kMaxDimensionPx = 4096;

  // 宽松但严格的 base64 字符集检查：非空、长度 %4==0、仅 [A-Za-z0-9+/=]、'=' 最多 2 个且只在尾部
  static bool isValidBase64(std::string_view s);
  // 解码失败返回 nullopt（先经 isValidBase64 检查）
  static std::optional<std::vector<uint8_t>> b64Decode(std::string_view s);
  // 按 magic + 维度解析类型；w/h 可为空；非法/SVG/超限返回 kReject
  static MediaKind detectKind(const std::vector<uint8_t>& bytes, int* w = nullptr, int* h = nullptr);
  // 解码→校验→HMAC 对象键落盘，成功返回 "/uploads/<64hex>.<ext>"
  static std::optional<std::string> decodeAndStore(std::string_view b64, const std::string& dir);

  // 单测辅助：base64 编码（便于构造 SVG 用例）
  static std::string b64EncodeForTest(const std::string& s);
};

}  // namespace im
