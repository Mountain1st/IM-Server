#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace im {

// 一条已存储的单聊消息（含状态字段）。
struct StoredMessage {
  std::string messageId;  // 全局唯一 ID（对外）
  int64_t fromUser;
  int64_t toUser;
  std::string content;
  int64_t ts;             // unix 秒
  std::string clientMsgId;
  std::string chatType;
  int status;             // 0=offline_pending 1=delivered 2=read
  int64_t groupId = 0;    // 0=单聊；>0=群聊（群消息单行：from=发送者, to=0）
  std::string msgType = "text";  // 消息类型：text=文本，image=图片（content 为 fileId）
  int64_t mediaId = 0;    // 0=无媒体；>0 关联 media.id
};

// 消息状态（与 messages 表 status 列一致）
enum MessageStatus {
  kMsgOfflinePending = 0,
  kMsgDelivered = 1,
  kMsgRead = 2,
  kMsgRecalled = 3,  // 撤回终态（msg.recall）：出站序列化剥离正文，仅发送者可撤
};

// 撤回结果分类（recallMessage：affected=0 时回查判定）
enum class RecallResult {
  kOk,        // 撤回成功（状态已迁移）
  kAlready,   // 该消息已被撤回（幂等成功）
  kNotFound,  // 消息不存在
  kForbidden, // 非发送者
  kExpired,   // 超过可撤回时间窗
  kError,     // 参数非法/DB 错误
};

// 撤回成功后供 notify 扇出的行信息（回查所得，chat_type/group_id/to_user 不可变）
struct RecallInfo {
  std::string chatType;  // "single" / "group"
  int64_t groupId = 0;   // 群聊 >0；单聊 0
  int64_t toUser = 0;    // 单聊对端（自己对话框 = 发送者自己）
};

// 会话列表条目（v3.2，单聊；群聊条目由 AppServer 层组装）
struct ConversationEntry {
  int64_t peerId = 0;     // 对端 userId
  int64_t lastMsgTs = 0;  // 收发双方向最大 ts
  int64_t unread = 0;     // 收方向未读数（status IN (0,1)）
  int64_t lastReadTs = 0; // 收方向已读游标（MAX(read_ts)）
};

// 会话末条消息信息（撤回语义经 recalled 透出，正文不回传）
struct LastMessageInfo {
  std::string messageId;
  std::string msgType;
  bool recalled = false;  // status == kMsgRecalled
};

// 消息存储接口：消息的持久化（在线/离线/历史）统一委托给本接口。
// 当前唯一实现为 MySQL（MySqlMessageRepository）。实现须自身保证线程安全
// （MySQL 单连接非线程安全，由实现内部加互斥锁串行化）。
class MessageRepository {
public:
  virtual ~MessageRepository() = default;

  // 存储是否可用（连接是否成功）。
  virtual bool ok() const = 0;

  // 保存一条消息。成功返回 true 并回填 messageId（如实现自生成则覆盖传入值）。
  virtual bool saveMessage(const std::string& messageId, int64_t fromUser,
                           int64_t toUser, const std::string& content,
                           int64_t ts, const std::string& clientMsgId,
                           const std::string& chatType, int status,
                           int64_t groupId = 0,
                           const std::string& msgType = "text",
                           int64_t mediaId = 0) = 0;
  // 取某接收方的离线消息（status=offline_pending），按 ts 升序。不移除。
  virtual std::vector<StoredMessage> fetchOffline(int64_t toUser) = 0;
  // 删除超过 TTL 仍未领取的单聊离线消息（ts < nowSec - 7 天）。返回删除条数。
  virtual int64_t purgeExpiredOffline(int64_t nowSec) = 0;
  // 保留期清理：删除超期的已送达/已读消息与全部群消息（ts < nowSec - 保留天数，
  // pending 状态不在此列——由 TTL 负责）。返回删除条数。
  virtual int64_t purgeAgedHistory(int64_t nowSec, int retentionDays) = 0;
  // 批量标记离线消息为 delivered（客户端 msg.offline_ack 确认）。
  virtual void markDelivered(int64_t toUser,
                             const std::vector<std::string>& messageIds) = 0;
  // 拉取 userA 与 userB 之间的单聊历史，按 ts 倒序（最新的在前），limit 条。
  virtual std::vector<StoredMessage> fetchHistory(int64_t userA, int64_t userB,
                                                  int limit) = 0;
  // 把接收方 toUser 与 fromUser 之间、readTs 之前未读的消息标记为已读。
  virtual void markRead(int64_t toUser, int64_t fromUser, int64_t readTs) = 0;
  // 撤回消息：单条 UPDATE 原子校验（存在 message_id + 发送者 from_user +
  // 时间窗 ts >= now-windowSec + 状态迁移 IN(0,1,2)→3 终态），免 TOCTOU。
  // affected=0 回查分类失败原因；kOk 时经 info 回传出站所需行信息
  // （notify 扇出用，含已撤回与已分类两种回查）。info 可为 nullptr。
  virtual RecallResult recallMessage(const std::string& messageId,
                                     int64_t senderId, int64_t nowSec,
                                     int64_t windowSec, RecallInfo* info) = 0;
  // 两用户之间是否存在单聊消息记录（任一方向）。read_notify 推送门槛用：
  // 无往来目标不推送，防 userId→account 枚举（2026-09-27 审计低危）。
  virtual bool hasConversation(int64_t userA, int64_t userB) = 0;
  // 媒体访问可达性（media.url 鉴权）：userId 是否为引用 fileId 的消息的可达方。
  // 单聊 from/to=userId；群聊 userId 为群现成员（left_ts=0）。
  // V1 不校验群消息可见时间窗口（保守放行，精确窗口属 V2）；查询失败 fail-closed。
  virtual bool hasMediaAccess(int64_t userId, const std::string& fileId) = 0;
  // 拉取某群消息：游标 (sinceTs, sinceId) 严格序（ts > sinceTs 或 ts==sinceTs 且 id>sinceId）；
  // untilTs==0 表示无上界；asc=true 按 (ts,id) 升序（补发），false 倒序（历史）；limit 条。
  virtual std::vector<StoredMessage> fetchGroupMessages(int64_t groupId,
                                                        int64_t sinceTs,
                                                        int64_t sinceId,
                                                        int64_t untilTs,
                                                        int limit, bool asc) = 0;

  // 按 message_id 反查 (ts, id, group_id)：群投递 ACK 游标推进用。
  // 找到返回 true（写出 ts/id/groupId）；不存在返回 false。
  virtual bool fetchMessageCursor(const std::string& messageId, int64_t* ts, int64_t* id,
                                  int64_t* groupId) = 0;

  // 会话列表（v3.2）：单聊条目，派生自 messages 事实源（收发双方向合并）。
  // unread=收方向 SUM(status IN (0,1))；lastReadTs=收方向 MAX(read_ts)；
  // 不含自己对话框（from/to=me 均排除）。群聊条目由 AppServer 层组装：
  // last_delivered_ts 是投递水位线而非已读游标，群未读 v3.2 恒 0（协议占位）。
  virtual std::vector<ConversationEntry> fetchSingleConversations(int64_t userId) = 0;

  // 会话末条消息（有界点查）：按 (ts,id) 严格序取最后一行（同秒边界安全）。
  // recalled=status==kMsgRecalled。无消息返回 false。
  virtual bool fetchLastMessageSingle(int64_t userA, int64_t userB,
                                      LastMessageInfo* out) = 0;
  virtual bool fetchLastMessageGroup(int64_t groupId, LastMessageInfo* out) = 0;

  // 各群末条消息 ts（一次 GROUP BY 聚合，供会话列表排序截断后再点查）。
  // 返回 (groupId, lastTs)；无消息的群不出现。
  virtual std::vector<std::pair<int64_t, int64_t>> fetchGroupLastTs(
      const std::vector<int64_t>& groupIds) = 0;
};

} // namespace im