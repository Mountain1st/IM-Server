-- 群聊管理 v2.0：groups 表补公告/解散四列（要求 MySQL 8.0+；对已存在的 groups 表执行）
-- 用法: MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_v2.0_group_mgmt.sql
-- 说明: db/schema.sql 仅对新库生效（CREATE TABLE IF NOT EXISTS 不给已存在的表加列）；
-- 本脚本为存量库一次性补列。可重复执行：逐列检查，已存在的列自动跳过（不会报
-- duplicate column），因此重复执行安全（幂等）。
-- 注意: announcement 为 TEXT NOT NULL 无默认值（MySQL 对 TEXT 默认值的限制），
-- ALTER 会给历史行填空串；announce_ts/dissolved/dissolved_ts 有 DEFAULT 0 自动填充。
-- 执行前先 SELECT 确认库身份（应能看到真实用户），禁 TRUNCATE/DROP。

-- 逐列条件添加：列不存在才 ALTER，存在则跳过
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'groups'
             AND COLUMN_NAME = 'announcement');
SET @sql := IF(@col = 0,
               'ALTER TABLE `groups` ADD COLUMN announcement TEXT NOT NULL',
               'SELECT ''announcement 已存在，跳过'' AS notice');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'groups'
             AND COLUMN_NAME = 'announce_ts');
SET @sql := IF(@col = 0,
               'ALTER TABLE `groups` ADD COLUMN announce_ts BIGINT NOT NULL DEFAULT 0',
               'SELECT ''announce_ts 已存在，跳过'' AS notice');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'groups'
             AND COLUMN_NAME = 'dissolved');
SET @sql := IF(@col = 0,
               'ALTER TABLE `groups` ADD COLUMN dissolved TINYINT NOT NULL DEFAULT 0',
               'SELECT ''dissolved 已存在，跳过'' AS notice');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'groups'
             AND COLUMN_NAME = 'dissolved_ts');
SET @sql := IF(@col = 0,
               'ALTER TABLE `groups` ADD COLUMN dissolved_ts BIGINT NOT NULL DEFAULT 0',
               'SELECT ''dissolved_ts 已存在，跳过'' AS notice');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
