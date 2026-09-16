-- 群聊管理 v2.0：groups 表补公告/解散四列（要求 MySQL 8.0+；对已存在的 groups 表执行）
-- 用法: MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_v2.0_group_mgmt.sql
-- 说明: db/schema.sql 仅对新库生效（CREATE TABLE IF NOT EXISTS 不给已存在的表加列）；
-- 本脚本为存量库一次性补列。先 SELECT 确认库身份再执行，禁 TRUNCATE/DROP。
-- 注意: announcement 为 TEXT NOT NULL 无默认值（MySQL 对 TEXT 默认值的限制），
-- ALTER 会给历史行填空串；announce_ts/dissolved/dissolved_ts 有 DEFAULT 0 自动填充。
ALTER TABLE `groups`
  ADD COLUMN announcement TEXT NOT NULL,
  ADD COLUMN announce_ts BIGINT NOT NULL DEFAULT 0,
  ADD COLUMN dissolved TINYINT NOT NULL DEFAULT 0,
  ADD COLUMN dissolved_ts BIGINT NOT NULL DEFAULT 0;
