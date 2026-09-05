#include "redis_token_store.hpp"

#include <cstdlib>

namespace im {

RedisTokenStore::RedisTokenStore(const RedisStoreConfig& cfg)
  : client_(cfg.redisHost, cfg.redisPort, cfg.redisPassword, cfg.redisDb) {}

void RedisTokenStore::put(const std::string& tokenDigest, int64_t userId,
                          std::chrono::seconds ttl) {
  client_.commandInt({"SETEX", tokenDigest, std::to_string(ttl.count()),
                      std::to_string(userId)});
}

int RedisTokenStore::get(const std::string& tokenDigest, int64_t* userId) {
  std::string v;
  if (!client_.commandString({"GET", tokenDigest}, &v)) return 1;
  if (v.empty()) return 1;
  *userId = std::strtoll(v.c_str(), nullptr, 10);
  return 0;
}

void RedisTokenStore::remove(const std::string& tokenDigest) {
  client_.commandInt({"DEL", tokenDigest});
}

} // namespace im