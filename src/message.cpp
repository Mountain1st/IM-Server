#include "message.hpp"

#include <utility>

namespace im {

bool parseEnvelope(const std::string& text, json* out) {
  json j;
  try {
    j = json::parse(text);
  } catch (...) {
    return false; // 非 JSON
  }
  // 必须是对象且包含 type(string) 与 data(object)
  if (!j.is_object()) return false;
  if (!j.contains(env::kType) || !j[env::kType].is_string()) return false;
  if (!j.contains(env::kData) || !j[env::kData].is_object()) return false;
  *out = std::move(j);
  return true;
}

json makeEnvelope(const std::string& type, json data) {
  json j = json::object();
  j[env::kType] = type;
  j[env::kData] = std::move(data);
  return j;
}

std::string makeEnvelopeText(const std::string& type, json data) {
  return makeEnvelope(type, std::move(data)).dump();
}

std::string makeError(int code, const std::string& msg) {
  return makeEnvelopeText("error", json{{"code", code}, {"msg", msg}});
}

std::string makeResult(const std::string& type, int code, const std::string& msg, json data) {
  json body = json::object();
  body["code"] = code;
  body["msg"] = msg;
  body["data"] = std::move(data);
  return makeEnvelopeText(type, std::move(body));
}

} // namespace im
