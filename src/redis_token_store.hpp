#pragma once

#include "redis_client.hpp"
#include "token_store.hpp"

#include <chrono>

namespace im {

// Redis token 存储：SETEX digest ttl userId / GET / DEL。
class RedisTokenStore : public TokenStore {
public:
  explicit RedisTokenStore(const RedisStoreConfig& cfg);
  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  void put(const std::string& tokenDigest, int64_t userId,
           std::chrono::seconds ttl) override;
  int get(const std::string& tokenDigest, int64_t* userId) override;
  void remove(const std::string& tokenDigest) override;

private:
  RedisClient client_;
};

} // namespace im