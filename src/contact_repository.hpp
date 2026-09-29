#pragma once

#include "mysql_client.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace im {

// 联系人/好友请求/用户隐私设置的 MySQL 存储。
// 仿 MySqlMessageRepository：单连接 + 内部互斥锁串行化 DB 访问。
// isContact 用内存缓存（contacts_ 集合）加速 msg.send 热路径，
// 启动全量加载；成对/删除时同步刷新缓存。
class ContactRepository {
public:
  ContactRepository(const std::string& host, unsigned port, const std::string& user,
                    const std::string& password, const std::string& db);

  bool ok() const { return client_.ok(); }
  std::string errStr() const { return client_.errStr(); }

  // ---- 联系人 ----
  bool isContact(int64_t a, int64_t b);                      // a 与 b 是否为联系人（走缓存）
  bool addContactPair(int64_t a, int64_t b);                 // 双向插入（幂等：已存在不报错）
  void addSelfContact(int64_t userId);                       // 自己联系人（INSERT IGNORE (uid,uid)）
  bool deleteContactPair(int64_t a, int64_t b);              // 双向删除
  std::vector<int64_t> listContactIds(int64_t userId);       // 我的联系人 userId 列表

  // ---- 好友请求 ----
  // 返回：0=成功新建；1=已存在 pending 或已接受（重复请求）；2=数据库错误。
  int addRequest(int64_t from, int64_t to);
  // 返回：0=成功接受（标记 accepted + 建立联系人关系）；1=无 pending 请求；2=数据库错误。
  int acceptRequest(int64_t to, int64_t from);
  // 查询某对好友请求的创建时间（Unix 秒）。找到 pending/任意状态行返回 true。
  bool requestTime(int64_t from, int64_t to, int64_t* ts);
  // 返回：0=成功拒绝；1=无 pending 请求；2=数据库错误。
  int rejectRequest(int64_t to, int64_t from);
  // 收到的 pending 请求列表（from_user, created_at 秒）
  struct PendingRequest {
    int64_t fromUser;
    int64_t ts;
  };
  std::vector<PendingRequest> listPendingRequests(int64_t to);

private:
  void loadContactsCache();
  // 锁内全量查询联系人关系到 out；查询失败返回 false（不动 out）
  bool fetchAllContacts(std::unordered_set<std::string>* out);

  MySqlClient client_;
  std::mutex mutex_; // 串行化 DB 访问 + 缓存读写
  // 已加载的 (a,b) 有向关系缓存；isContact(a,b) 查 (a,b)
  std::unordered_set<std::string> contacts_; // 键格式 "a|b"

public:
  // ---- 缓存低频比对（防绕过服务端直改库导致缓存漂移）----
  // 库内联系人总行数；查询失败返回 false（调用方跳过本轮比对）
  bool dbContactCount(size_t* out);
  size_t cacheContactCount();  // 内存缓存条目数（锁内）
  // 全量重建缓存：先查到临时集合、成功才换入（失败保持旧缓存，不清空）
  void reloadContactsCache();
};

} // namespace im
