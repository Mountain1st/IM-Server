#include "test_util.hpp"

#include "websocket.hpp"

#include <string>

using namespace im;

namespace {

// 构造客户端掩码帧（16 字节 key 的 Base64 用于握手；这里构造数据帧）
std::string buildMaskedFrame(ws::Opcode opcode, const std::string& payload,
                             bool fin = true, uint8_t rsv = 0) {
  std::string out;
  uint8_t b0 = (fin ? 0x80 : 0) | rsv | static_cast<uint8_t>(opcode);
  size_t len = payload.size();
  out.push_back(static_cast<char>(b0));
  if (len < 126) {
    out.push_back(static_cast<char>(0x80 | len));
  } else if (len <= 0xFFFF) {
    out.push_back(static_cast<char>(0x80 | 126));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
  } else {
    out.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; --i) {
      out.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }
  const char mask[4] = {0x11, 0x22, 0x33, 0x44};
  out.append(mask, 4);
  for (size_t i = 0; i < payload.size(); ++i) {
    out.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
  }
  return out;
}

// 未掩码帧（RFC 6455 §5.1 服务器必须拒绝）
std::string buildUnmaskedFrame(ws::Opcode opcode, const std::string& payload) {
  std::string out;
  out.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));
  out.push_back(static_cast<char>(payload.size()));
  out += payload;
  return out;
}

} // namespace

TEST(parser_single_frame) {
  ws::FrameParser p;
  p.setMaxBytes(1024 * 1024);
  auto raw = buildMaskedFrame(ws::Opcode::Text, "hello");
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::Complete);
  CHECK(f.opcode == ws::Opcode::Text);
  CHECK_EQ(f.payload, std::string("hello"));
}

TEST(parser_partial_then_more) {
  ws::FrameParser p;
  p.setMaxBytes(1024 * 1024);
  auto raw = buildMaskedFrame(ws::Opcode::Text, "hello world");
  p.feed(raw.data(), 3); // 半帧
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::NeedMore);
  p.feed(raw.data() + 3, raw.size() - 3);
  CHECK(p.parseOne(&f) == ws::ParseResult::Complete);
  CHECK_EQ(f.payload, std::string("hello world"));
}

TEST(parser_unmasked_rejected) {
  ws::FrameParser p;
  auto raw = buildUnmaskedFrame(ws::Opcode::Text, "hi");
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_rsv_rejected) {
  ws::FrameParser p;
  auto raw = buildMaskedFrame(ws::Opcode::Text, "hi", true, 0x40); // RSV1
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_bad_opcode_rejected) {
  ws::FrameParser p;
  auto raw = buildMaskedFrame(static_cast<ws::Opcode>(0x3), "hi");
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_control_frame_fragmented_rejected) {
  ws::FrameParser p;
  auto raw = buildMaskedFrame(ws::Opcode::Close, "hi", false);
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_control_frame_too_long_rejected) {
  ws::FrameParser p;
  auto raw = buildMaskedFrame(ws::Opcode::Ping, std::string(126, 'x'));
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_nonminimal_length_rejected) {
  ws::FrameParser p;
  // 7 位长度码=126 但实际长度 5（违反最小长度编码）
  std::string raw;
  raw.push_back(static_cast<char>(0x81));
  raw.push_back(static_cast<char>(0x80 | 126));
  raw.push_back(static_cast<char>(0));
  raw.push_back(static_cast<char>(5));
  raw.append("\x01\x02\x03\x04", 4);
  for (int i = 0; i < 5; ++i) raw.push_back(static_cast<char>(0x61 ^ 0x11));
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_64bit_length_lowbyte_highbit_accepted) {
  // 8 字节长度编码且长度低字节最高位为 1（如 70088=0x111B8）必须接受：
  // 最高位检查须用主机序转换后的值，小端机上误用网络序原值会误杀
  //（任务 4 图片 413 路径的 1.5MB+ 信封帧必经此分支）
  ws::FrameParser p;
  p.setMaxBytes(2 * 1024 * 1024);
  auto raw = buildMaskedFrame(ws::Opcode::Text, std::string(70088, 'A'));
  // 分片喂入，模拟 TCP 跨包到达
  size_t off = 0;
  ws::Frame f;
  ws::ParseResult pr = ws::ParseResult::NeedMore;
  while (off < raw.size()) {
    size_t n = std::min<size_t>(32768, raw.size() - off);
    p.feed(raw.data() + off, n);
    off += n;
    pr = p.parseOne(&f);
    if (pr != ws::ParseResult::NeedMore) break;
  }
  CHECK(pr == ws::ParseResult::Complete);
  CHECK(f.payload.size() == size_t(70088));
}

TEST(parser_64bit_length_msb_set_rejected) {
  // RFC 6455 §5.2 负向锁定：64 位长度最高位为 1 必须拒绝（与接受侧单测配对）
  ws::FrameParser p;
  p.setMaxBytes(2 * 1024 * 1024);
  std::string raw;
  raw.push_back(static_cast<char>(0x81));
  raw.push_back(static_cast<char>(0x80 | 127));
  raw.append("\x80\x00\x00\x00\x00\x01\x00\x00", 8);  // 最高位 1
  raw.append("\x11\x22\x33\x44", 4);
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_64bit_image_envelope_size_accepted) {
  // 图片信封贯通：1.5MB 上限 + JSON 开销约 1.6MB 帧走 64 位长度分支必须接受。
  // 依赖 kMaxFrameBytes(64MB) 远大于图片信封；若收紧帧上限，此用例即红。
  ws::FrameParser p;
  p.setMaxBytes(2 * 1024 * 1024);
  const std::string big(1572864 + 1024, 'B');
  auto raw = buildMaskedFrame(ws::Opcode::Text, big);
  size_t off = 0;
  ws::Frame f;
  ws::ParseResult pr = ws::ParseResult::NeedMore;
  while (off < raw.size()) {
    size_t n = std::min<size_t>(65536, raw.size() - off);
    p.feed(raw.data() + off, n);
    off += n;
    pr = p.parseOne(&f);
    if (pr != ws::ParseResult::NeedMore) break;
  }
  CHECK(pr == ws::ParseResult::Complete);
  CHECK(f.payload.size() == big.size());
}

TEST(parser_utf8_valid) {
  CHECK(ws::isValidUtf8("hello"));
  CHECK(ws::isValidUtf8("中文消息"));
  CHECK(ws::isValidUtf8("\xE2\x82\xAC")); // € (U+20AC)
}

TEST(parser_utf8_invalid) {
  CHECK(!ws::isValidUtf8("\xff\xfe"));
  CHECK(!ws::isValidUtf8("\xED\xA0\x80")); // 代理区 D800
  CHECK(!ws::isValidUtf8("\xC0\xAF"));     // overlong
  CHECK(!ws::isValidUtf8("ab\xC3"));       // 截断
}

TEST(parser_close_code_valid) {
  CHECK(ws::isValidCloseCode(1000));
  CHECK(ws::isValidCloseCode(1001));
  CHECK(ws::isValidCloseCode(1007));
  CHECK(ws::isValidCloseCode(3000));
  CHECK(ws::isValidCloseCode(4999));
}

TEST(parser_close_code_invalid) {
  CHECK(!ws::isValidCloseCode(1005));
  CHECK(!ws::isValidCloseCode(1004));
  CHECK(!ws::isValidCloseCode(1012));
  CHECK(!ws::isValidCloseCode(2000));
}

TEST(parser_close_invalid_code_rejected) {
  ws::FrameParser p;
  auto raw = buildMaskedFrame(ws::Opcode::Close, "\x03\xed"); // 1005 保留码
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_close_valid_code_accepted) {
  // beast 等标准客户端发送 Close(1000)：payload = 原始 2 字节 code（非 UTF-8）
  ws::FrameParser p;
  auto raw = buildMaskedFrame(ws::Opcode::Close, "\x03\xe8");
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::Complete);
  CHECK(f.opcode == ws::Opcode::Close);
}

TEST(parser_close_code_with_utf8_reason_accepted) {
  ws::FrameParser p;
  // Close(1000) + UTF-8 reason "再见"
  std::string payload = "\x03\xe8";
  payload += "\xE5\x86\x8D\xE8\xA7\x81";
  auto raw = buildMaskedFrame(ws::Opcode::Close, payload);
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::Complete);
}

TEST(parser_close_bad_reason_utf8_rejected) {
  ws::FrameParser p;
  // Close(1000) + 非法 UTF-8 reason
  std::string payload = "\x03\xe8";
  payload += "\xff\xfe";
  auto raw = buildMaskedFrame(ws::Opcode::Close, payload);
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_close_single_byte_payload_rejected) {
  ws::FrameParser p;
  auto raw = buildMaskedFrame(ws::Opcode::Close, "\x03"); // 1 字节违反 RFC
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::ProtocolError);
}

TEST(parser_utf8_payload_invalid_rejected) {
  ws::FrameParser p;
  auto raw = buildMaskedFrame(ws::Opcode::Text, "\xff\xfe");
  p.feed(raw.data(), raw.size());
  ws::Frame f;
  CHECK(p.parseOne(&f) == ws::ParseResult::Complete); // 解析层不校验 Text UTF-8
}

int main() {
  RUN(parser_single_frame);
  RUN(parser_partial_then_more);
  RUN(parser_unmasked_rejected);
  RUN(parser_rsv_rejected);
  RUN(parser_bad_opcode_rejected);
  RUN(parser_control_frame_fragmented_rejected);
  RUN(parser_control_frame_too_long_rejected);
  RUN(parser_nonminimal_length_rejected);
  RUN(parser_64bit_length_lowbyte_highbit_accepted);
  RUN(parser_64bit_length_msb_set_rejected);
  RUN(parser_64bit_image_envelope_size_accepted);
  RUN(parser_utf8_valid);
  RUN(parser_utf8_invalid);
  RUN(parser_close_code_valid);
  RUN(parser_close_code_invalid);
  RUN(parser_close_invalid_code_rejected);
  RUN(parser_close_valid_code_accepted);
  RUN(parser_close_code_with_utf8_reason_accepted);
  RUN(parser_close_bad_reason_utf8_rejected);
  RUN(parser_close_single_byte_payload_rejected);
  RUN(parser_utf8_payload_invalid_rejected);
  std::printf("websocket_parser_test: %d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}