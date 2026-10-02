-- 迁移：media 表 + messages.media_id 列（媒体系统 V1）
-- 背景：图片从 base64/本地盘升级为对象存储引用，messages 只存 media_id；
-- 历史数据 media_id 默认 NULL，保持回读兼容。
-- 幂等：media 表 IF NOT EXISTS；media_id 列查询 information_schema.COLUMNS 判定后加列。
-- 用法：MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_media.sql

-- 媒体表：对象存储引用与元数据
CREATE TABLE IF NOT EXISTS media (
  id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  file_id       CHAR(32)     NOT NULL,
  uploader_id   BIGINT UNSIGNED NOT NULL,
  object_key    VARCHAR(255) NOT NULL,
  file_name     VARCHAR(255) NOT NULL,
  mime_type     VARCHAR(64)  NOT NULL,
  file_size     BIGINT UNSIGNED NOT NULL DEFAULT 0,
  width         INT          NOT NULL DEFAULT 0,
  height        INT          NOT NULL DEFAULT 0,
  duration      INT          NOT NULL DEFAULT 0,
  thumb_key     VARCHAR(255) NOT NULL DEFAULT '',
  status        TINYINT      NOT NULL DEFAULT 0,  -- 0=PENDING 1=UPLOADED 2=COMMITTED 3=GC
  created_at    BIGINT       NOT NULL DEFAULT 0,
  UNIQUE KEY uk_file_id (file_id),
  UNIQUE KEY uk_object_key (object_key),
  KEY idx_uploader (uploader_id),
  KEY idx_status_created (status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- messages.media_id（幂等）
SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='messages' AND column_name='media_id');
SET @s := IF(@c=0,
  'ALTER TABLE messages ADD COLUMN media_id BIGINT UNSIGNED NULL, ADD KEY idx_media (media_id)',
  'SELECT ''skip media_id''');
PREPARE stmt FROM @s; EXECUTE stmt; DEALLOCATE PREPARE stmt;
