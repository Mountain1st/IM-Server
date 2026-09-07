-- IM 服务器数据库初始化脚本（MySQL 8.x）
-- 实际部署数据库名/账号以连接配置为准；本文件建库默认 IMS_reactor_db。
-- 用法: MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 < db/schema.sql
--
-- 用户模型：
--   id        = userId：系统分配的数字唯一标识，消息收发目标（msg.send.to / msg.recv.from）
--   account   = 登录账号（唯一，登录凭据）
--   username  = 昵称（展示名，可重复，缺省与账号一致）

CREATE DATABASE IF NOT EXISTS IMS_reactor_db
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE IMS_reactor_db;

CREATE TABLE IF NOT EXISTS users (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  account       VARCHAR(64)  NOT NULL UNIQUE,
  username      VARCHAR(64)  NOT NULL,
  salt          CHAR(32)     NOT NULL,           -- 16 字节随机盐的 hex
  password_hash CHAR(64)     NOT NULL,           -- PBKDF2-HMAC-SHA256 32 字节 hex
  created_at    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  allow_temp_chat TINYINT    NOT NULL DEFAULT 1, -- 允许非联系人临时对话
  searchable      TINYINT    NOT NULL DEFAULT 1, -- 可被搜索（user.search）
  allow_add       TINYINT    NOT NULL DEFAULT 1  -- 可被添加（contact.request）
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 消息表：单聊消息的权威存储（在线投递 + 离线补发 + 历史查询）。
-- status 状态机：
--   0 = OFFLINE_PENDING  目标离线，待补发
--   1 = DELIVERED        已投递（在线投递 或 离线消息已由客户端确认）
--   2 = READ             接收方已读（msg.read）
CREATE TABLE IF NOT EXISTS messages (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  message_id    VARCHAR(64)  NOT NULL,           -- 全局唯一 ID（对外，供确认/去重）
  from_user     BIGINT UNSIGNED NOT NULL,
  to_user       BIGINT UNSIGNED NOT NULL,
  content       MEDIUMTEXT   NOT NULL,
  ts            BIGINT       NOT NULL,           -- unix 时间戳（秒）
  client_msg_id VARCHAR(64)  NOT NULL DEFAULT '',
  chat_type     VARCHAR(16)  NOT NULL DEFAULT 'single',
  group_id      BIGINT UNSIGNED NOT NULL DEFAULT 0,
  status        TINYINT      NOT NULL DEFAULT 0,
  delivered_ts  BIGINT       NOT NULL DEFAULT 0,
  read_ts       BIGINT       NOT NULL DEFAULT 0,
  KEY idx_to_status (to_user, status),
  KEY idx_pair_ts  (from_user, to_user, ts),
  KEY idx_message_id (message_id),
  KEY idx_group_ts (group_id, ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 联系人关系表（双向各一条，删除时删两条）
CREATE TABLE IF NOT EXISTS contacts (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id    BIGINT UNSIGNED NOT NULL,
  contact_id BIGINT UNSIGNED NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_pair (user_id, contact_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 好友请求表：0=pending 1=accepted 2=rejected
CREATE TABLE IF NOT EXISTS friend_requests (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  from_user  BIGINT UNSIGNED NOT NULL,
  to_user    BIGINT UNSIGNED NOT NULL,
  status     TINYINT NOT NULL DEFAULT 0,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_from_to (from_user, to_user)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `groups` (
  group_id    BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name        VARCHAR(32)  NOT NULL,
  owner_id    BIGINT UNSIGNED NOT NULL,
  created_ts  BIGINT       NOT NULL,
  KEY idx_owner (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS group_members (
  id                BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_id          BIGINT UNSIGNED NOT NULL,
  user_id           BIGINT UNSIGNED NOT NULL,
  joined_ts         BIGINT       NOT NULL,
  left_ts           BIGINT       NOT NULL DEFAULT 0,  -- 0=在群，非0=离开时间
  last_delivered_ts BIGINT       NOT NULL DEFAULT 0,  -- 离线补发水位线
  UNIQUE KEY uk_group_user (group_id, user_id),
  KEY idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
