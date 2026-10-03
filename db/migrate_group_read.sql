-- 迁移：群已读游标（v3.4）——group_members 增 (last_read_ts, last_read_id)
-- 背景：投递游标（last_delivered_*）是「已收到」，已读游标是「已阅读」——
-- 两者语义独立，群未读/已读角标需要成员级已读位置（用户级，非设备级）。
-- 存量行默认 0（视为全部未读；客户端首次 group.read 后即归零）。
-- 幂等：information_schema 判定后加列，可重复执行。
-- 用法：MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_group_read.sql

SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='group_members'
             AND column_name='last_read_ts');
SET @s := IF(@c = 0,
  'ALTER TABLE group_members ADD COLUMN last_read_ts BIGINT NOT NULL DEFAULT 0',
  'SELECT 1');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='group_members'
             AND column_name='last_read_id');
SET @s := IF(@c = 0,
  'ALTER TABLE group_members ADD COLUMN last_read_id BIGINT UNSIGNED NOT NULL DEFAULT 0',
  'SELECT 1');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

-- 入群边界 id 分量（与 joined_ts 组成 (ts,id) 严格边界）：
-- 同秒入群时「入群前消息」与 joined_ts 同秒，ts-only 窗口会误计未读；
-- id 唯一且自增，配合 joined_ts 精确排除入群前历史（不变量 I3）。
SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='group_members'
             AND column_name='joined_boundary_id');
SET @s := IF(@c = 0,
  'ALTER TABLE group_members ADD COLUMN joined_boundary_id BIGINT UNSIGNED NOT NULL DEFAULT 0',
  'SELECT 1');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
