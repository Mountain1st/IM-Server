#pragma once
// appserver 共享小工具（原 appserver.cpp 匿名 namespace）：
// 账号/密码格式校验 + 消息收发目标解析。header-only，供单测直接包含。
#include "message.hpp"        // json
#include "server_config.hpp"
#include "user_store.hpp"     // UserStore（resolveTarget 经其查存在性/账号）

#include <algorithm>
#include <cstdint>
#include <string>

namespace im {

// 业务参数限制统一取自集中配置（server_config.hpp）
constexpr size_t kMaxContentBytes = config::kMaxContentBytes;
constexpr size_t kMinAccountLen = config::kMinAccountLen;
constexpr size_t kMaxAccountLen = config::kMaxAccountLen;
constexpr size_t kLoginAccountMaxLen = config::kLoginAccountMaxLen;
constexpr size_t kMaxUsernameLen = config::kMaxUsernameLen;
constexpr size_t kMinPasswordLen = config::kMinPasswordLen;
constexpr size_t kMaxPasswordLen = config::kMaxPasswordLen;
// msg.send 限流：窗口时长内每用户最多发送条数；空 clientMsgId 无法参与去重，走更严限制
constexpr std::chrono::seconds kSendWindow(config::kSendWindow);
constexpr size_t kMaxSendPerWindow = config::kMaxSendPerWindow;
constexpr size_t kMaxSendNoIdPerWindow = config::kMaxSendNoIdPerWindow;
constexpr size_t kMaxClientMsgIdBytes = config::kMaxClientMsgIdBytes;
constexpr size_t kMaxOfflineAckIds = config::kMaxOfflineAckIds;
constexpr std::chrono::seconds kQueryWindow(config::kQueryWindow);
constexpr size_t kQueryLimitPerWindow = config::kQueryLimitPerWindow;

// 账号格式：10-11 位纯数字（如手机号）。
// 注册与新建对话（chat.create）均按此校验，不合规回 400；
// 登录不校验格式（错误统一按 403 处理，兼容遗留账号，不泄露账号存在性）。
inline bool validAccount(const std::string& a) {
  if (a.size() < kMinAccountLen || a.size() > kMaxAccountLen) return false;
  for (char c : a) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// 密码规则校验：6-32 位、仅字母/数字、且同时包含字母与数字
inline bool validPassword(const std::string& p) {
  if (p.size() < kMinPasswordLen || p.size() > kMaxPasswordLen) return false;
  bool hasLetter = false, hasDigit = false;
  for (unsigned char c : p) {
    if (c >= '0' && c <= '9') {
      hasDigit = true;
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      hasLetter = true;
    } else {
      return false; // 含非字母数字字符
    }
  }
  return hasLetter && hasDigit; // 必须字母与数字都有
}

// 解析消息收发目标字段（msg.send 的 to / chat.history 的 to / msg.read 的 from）。
// 协议 v1.8：字段为登录账号 account；
// 兼容旧协议传数字 userId。自己对话框通过 to==自己 account 表达，
// 不再有 to:0 特例。
// 返回值：>0=目标 userId；-1=参数错误（400）；-2=目标不存在（404）；-3=存储异常（500）。
inline int64_t resolveTarget(UserStore* store, const json& data, const char* key) {
  if (!data.contains(key)) return -1;
  const json& v = data[key];
  std::string s;
  if (v.is_string()) {
    s = v.get<std::string>();
  } else if (v.is_number_integer()) {
    s = std::to_string(v.get<int64_t>());
  } else {
    return -1;
  }
  if (s.empty()) return -1;
  const bool digits = std::all_of(s.begin(), s.end(),
                                  [](char c) { return c >= '0' && c <= '9'; });
  // 非 10-11 位数字：视为旧协议 userId 数字（如 121），按 userId 校验
  if (!digits || s.size() < kMinAccountLen || s.size() > kMaxAccountLen) {
    int64_t uid = 0;
    try {
      uid = std::stoll(s);
    } catch (...) {
      return -1;
    }
    if (uid <= 0) return -1;
    if (!store->exists(uid)) return -2;
    return uid;
  }
  // 10-11 位数字：按登录账号 account 解析
  int64_t uid = 0;
  std::string nick;
  const int frc = store->lookupAccount(s, &uid, &nick);
  if (frc == 2) return -3;
  if (frc != 0) return -2;
  return uid;
}

}  // namespace im
