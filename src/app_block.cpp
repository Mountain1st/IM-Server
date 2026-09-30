// ============================================================
// 业务域：拉黑/黑名单（block.*，v2.4）
// 语义：双向断联——A 拉黑 B 期间双方单聊/好友请求全拒（文案分方向，
// 被拉黑方文案模糊防探测）；群聊不受影响；历史与已落库消息不追溯
// （拉黑前的 pending 离线消息照常补发——不追溯到底）。
// 解除后恢复通信能力，不恢复联系人关系（需重新走好友请求）。
// onBlockAdd 联动顺序（数据一致性关键）：先写 blocks（主操作，
// 409 幂等检查前置）→ 成功后再尽力联动（删联系人/双向请求置
// rejected）——联动失败仅记日志，拉黑已成立；颠倒顺序会在中途
// 失败时留下"删了好友却没拉黑成"的中间态
// 线程：工作线程池（AppServer::onEnvelope 分发）
// ============================================================
#include "appserver.hpp"

#include "app_util.hpp"
#include "log.hpp"
#include "session.hpp"

namespace im {

void AppServer::onBlockAdd(const std::shared_ptr<Session>& session, const json& data) {
  const int64_t to = resolveTarget(userStore_.get(), data, "account");
  if (to == -1) {
    session->reply(makeResult("block.add_res", 400, "参数错误", json::object()));
    return;
  }
  if (to == -2) {
    session->reply(makeResult("block.add_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (to == -3) {
    session->reply(makeResult("block.add_res", 500, "服务器内部错误", json::object()));
    return;
  }
  const int64_t self = session->userId();
  if (to == self) {
    session->reply(makeResult("block.add_res", 400, "不能拉黑自己", json::object()));
    return;
  }
  // ① 主操作：写 blocks（幂等检查靠 uk_pair 唯一键 → 409）
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int rc = blockRepo_->addBlock(self, to, now);
  if (rc == 1) {
    session->reply(makeResult("block.add_res", 409, "已拉黑对方", json::object()));
    return;
  }
  if (rc != 0) {
    session->reply(makeResult("block.add_res", 500, "服务器内部错误", json::object()));
    return;
  }
  // ② 尽力联动（失败仅日志，拉黑已成立）：
  // 若为联系人 → 双向删除；双向好友请求置 rejected（允许解除后重新申请）
  if (contactRepo_->isContact(self, to)) {
    if (!contactRepo_->deleteContactPair(self, to)) {
      LOG_E("[block] 拉黑联动删除联系人失败 user=" << self << " target=" << to);
    }
  }
  contactRepo_->rejectRequestsBetween(self, to);
  session->reply(makeResult("block.add_res", 0, "ok", json::object()));
}

void AppServer::onBlockRemove(const std::shared_ptr<Session>& session, const json& data) {
  const int64_t to = resolveTarget(userStore_.get(), data, "account");
  if (to == -1) {
    session->reply(makeResult("block.remove_res", 400, "参数错误", json::object()));
    return;
  }
  if (to == -2) {
    session->reply(makeResult("block.remove_res", 404, "目标用户不存在", json::object()));
    return;
  }
  if (to == -3) {
    session->reply(makeResult("block.remove_res", 500, "服务器内部错误", json::object()));
    return;
  }
  const int64_t self = session->userId();
  if (to == self) {
    session->reply(makeResult("block.remove_res", 400, "参数错误", json::object()));
    return;
  }
  const int rc = blockRepo_->removeBlock(self, to);
  if (rc == 1) {
    session->reply(makeResult("block.remove_res", 409, "未拉黑对方", json::object()));
    return;
  }
  if (rc != 0) {
    session->reply(makeResult("block.remove_res", 500, "服务器内部错误", json::object()));
    return;
  }
  // 解除恢复通信能力；联系人关系不恢复（需重新走好友请求流程）
  session->reply(makeResult("block.remove_res", 0, "ok", json::object()));
}

void AppServer::onBlockList(const std::shared_ptr<Session>& session, const json& data) {
  (void)data; // 无参数
  const auto entries = blockRepo_->listBlocked(session->userId());
  json arr = json::array();
  for (const auto& [id, ts] : entries) {
    std::string account, username;
    userStore_->accountOf(id, &account);
    userStore_->nickname(id, &username);
    arr.push_back(json{{"account", account}, {"username", username}, {"ts", ts}});
  }
  session->reply(makeResult("block.list_res", 0, "ok", json{{"blocks", arr}}));
}

} // namespace im
