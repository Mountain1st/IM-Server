-- 迁移：新增拉黑表（v2.4 黑名单功能，2026-10-01）
-- 幂等：CREATE TABLE IF NOT EXISTS，重复执行零副作用；
-- 新增表不影响现有数据卷，可先于代码上线（向后兼容）。
-- 用法：mysql -u root -p IMS_reactor_db < db/migrate_blocks.sql

CREATE TABLE IF NOT EXISTS blocks (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id    BIGINT UNSIGNED NOT NULL,   -- 拉黑方
  blocked_id BIGINT UNSIGNED NOT NULL,   -- 被拉黑方
  created_ts BIGINT       NOT NULL,
  UNIQUE KEY uk_pair (user_id, blocked_id),
  KEY idx_blocked (blocked_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
