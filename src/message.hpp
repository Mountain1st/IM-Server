#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace im {

using json = nlohmann::json;

// 信封字段名常量，统一引用避免手写字符串出错
namespace env {
constexpr char kType[] = "type";
constexpr char kSeq[]  = "seq";
constexpr char kTs[]   = "ts";
constexpr char kData[] = "data";
} // namespace env

// 解析客户端文本帧为信封对象（协议见 docs/protocol.md）。
// 返回 true  表示合法封包（JSON 对象、含 type(string)/data(object)），结果写入 out；
// 返回 false 表示非法封包，out 内容无效。
bool parseEnvelope(const std::string& text, json* out);

// 构造信封对象（data 缺省为空对象）
json makeEnvelope(const std::string& type, json data = json::object());

// 构造信封对象并序列化为待发送的文本帧
std::string makeEnvelopeText(const std::string& type, json data = json::object());

// 构造通用错误事件文本帧：{ type:"error", data:{ code, msg } }
std::string makeError(int code, const std::string& msg);

// 构造业务响应文本帧：data 内自动包装 { code, msg, data }
// 对应协议中 auth.*_res / msg.ack 的结构
std::string makeResult(const std::string& type, int code, const std::string& msg, json data = json::object());

} // namespace im
