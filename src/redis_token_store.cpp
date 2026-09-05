#include "redis_token_store.hpp"

#include <cstdlib>
#include <iostream>

namespace im {

RedisClient::RedisClient(const std::string& host, unsigned port,
                         const std::string& password, int db)
  : host_(host), port_(port), password_(password), db_(db) {
  connect();
}

RedisClient::~RedisClient() {
  if (ctx_) redisFree(ctx_);
}

void RedisClient::connect() {
  if (ctx_) {
    redisFree(ctx_);
    ctx_ = nullptr;
  }
  ctx_ = redisConnect(host_.c_str(), port_);
  if (!ctx_ || ctx_->err) {
    errStr_ = ctx_ ? ctx_->errstr : "allocation failure";
    ok_ = false;
    if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
    return;
  }
  // AUTH 与 SELECT
  if (!password_.empty()) {
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "AUTH %s", password_.c_str()));
    if (!r || r->type == REDIS_REPLY_ERROR) {
      errStr_ = r && r->str ? r->str : "AUTH failed";
      ok_ = false;
      if (r) freeReplyObject(r);
      redisFree(ctx_); ctx_ = nullptr;
      return;
    }
    freeReplyObject(r);
  }
  if (db_ != 0) {
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "SELECT %d", db_));
    if (!r || r->type == REDIS_REPLY_ERROR) {
      errStr_ = r && r->str ? r->str : "SELECT failed";
      ok_ = false;
      if (r) freeReplyObject(r);
      redisFree(ctx_); ctx_ = nullptr;
      return;
    }
    freeReplyObject(r);
  }
  ok_ = true;
  errStr_.clear();
}

namespace {
// 释放一条命令 reply（若 pending 则丢弃，避免干扰后续命令）
void discardReply(redisReply* r) {
  if (r) freeReplyObject(r);
}
} // namespace

int64_t RedisClient::commandInt(const std::vector<std::string>& args) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ctx_ || !ok_) return -2;
  // 用 redisCommandArgv 构造参数化命令（避免值含特殊字符被解析为命令）
  std::vector<const char*> argv(args.size());
  std::vector<size_t> lens(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    argv[i] = args[i].data();
    lens[i] = args[i].size();
  }
  redisReply* r = static_cast<redisReply*>(
      redisCommandArgv(ctx_, static_cast<int>(args.size()), argv.data(), lens.data()));
  if (!r) {
    // 连接可能已断，标记失败；下次命令前重建
    ok_ = false;
    errStr_ = ctx_->errstr;
    return -2;
  }
  if (r->type == REDIS_REPLY_ERROR) {
    errStr_ = r->str ? r->str : "redis error";
    discardReply(r);
    return -2;
  }
  int64_t v = r->integer;
  discardReply(r);
  return v;
}

bool RedisClient::commandString(const std::vector<std::string>& args,
                                std::string* value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ctx_ || !ok_) return false;
  std::vector<const char*> argv(args.size());
  std::vector<size_t> lens(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    argv[i] = args[i].data();
    lens[i] = args[i].size();
  }
  redisReply* r = static_cast<redisReply*>(
      redisCommandArgv(ctx_, static_cast<int>(args.size()), argv.data(), lens.data()));
  if (!r) {
    ok_ = false;
    errStr_ = ctx_->errstr;
    return false;
  }
  if (r->type == REDIS_REPLY_ERROR) {
    errStr_ = r->str ? r->str : "redis error";
    discardReply(r);
    return false;
  }
  if (r->type == REDIS_REPLY_NIL) { // 不存在
    discardReply(r);
    value->clear();
    return true;
  }
  if (r->type == REDIS_REPLY_STRING) {
    *value = std::string(r->str, r->len);
    discardReply(r);
    return true;
  }
  discardReply(r);
  return false;
}

RedisTokenStore::RedisTokenStore(const RedisStoreConfig& cfg)
  : client_(cfg.redisHost, cfg.redisPort, cfg.redisPassword, cfg.redisDb) {}

void RedisTokenStore::put(const std::string& tokenDigest, int64_t userId,
                          std::chrono::seconds ttl) {
  client_.commandInt({"SETEX", tokenDigest, std::to_string(ttl.count()),
                      std::to_string(userId)});
}

int RedisTokenStore::get(const std::string& tokenDigest, int64_t* userId) {
  std::string v;
  if (!client_.commandString({"GET", tokenDigest}, &v)) return 1; // 连接失败按无效处理
  if (v.empty()) return 1;
  *userId = std::strtoll(v.c_str(), nullptr, 10);
  return 0;
}

void RedisTokenStore::remove(const std::string& tokenDigest) {
  client_.commandInt({"DEL", tokenDigest});
}

} // namespace im