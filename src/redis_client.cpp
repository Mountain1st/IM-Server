#include "redis_client.hpp"

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

redisReply* RedisClient::execLocked(const std::vector<std::string>& args) {
  // 前提：已持有 mutex_（commandInt/commandString/commandStatus 均先加锁）
  if (!ctx_ || !ok_) {
    connect(); // 断线重建
    if (!ok_) return nullptr;
  }
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
    return nullptr;
  }
  if (r->type == REDIS_REPLY_ERROR) {
    errStr_ = r->str ? r->str : "redis error";
    freeReplyObject(r);
    return nullptr;
  }
  return r;
}

int64_t RedisClient::commandInt(const std::vector<std::string>& args) {
  std::lock_guard<std::mutex> lock(mutex_);
  redisReply* r = execLocked(args);
  if (!r) return -2;
  int64_t v = r->integer;
  freeReplyObject(r);
  return v;
}

bool RedisClient::commandString(const std::vector<std::string>& args,
                                std::string* value) {
  std::lock_guard<std::mutex> lock(mutex_);
  redisReply* r = execLocked(args);
  if (!r) return false;
  if (r->type == REDIS_REPLY_NIL) {
    freeReplyObject(r);
    value->clear();
    return true;
  }
  if (r->type == REDIS_REPLY_STRING) {
    *value = std::string(r->str, r->len);
    freeReplyObject(r);
    return true;
  }
  freeReplyObject(r);
  return false;
}

int64_t RedisClient::commandStatus(const std::vector<std::string>& args) {
  std::lock_guard<std::mutex> lock(mutex_);
  redisReply* r = execLocked(args);
  if (!r) return -2;
  // OK 状态（如 SET 无 NX）与整数结果（SET NX 返回 1/0）统一按整数语义返回
  int64_t v = r->type == REDIS_REPLY_STATUS ? 1 : r->integer;
  freeReplyObject(r);
  return v;
}

} // namespace im