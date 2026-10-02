-- 迁移：投递确认语义 v2（P0.5）——group_members 增有序投递游标 id
-- 背景：群投递游标从秒级 last_delivered_ts 升级为 (last_delivered_ts, last_delivered_id)
-- 严格有序游标，由客户端 group.offline_ack 累计推进（只进不退）。
-- 存量行默认 0（不追补历史；新逻辑仅从部署后按 ACK 严格推进）。
-- 幂等：information_schema 判定后加列，可重复执行。
-- 用法：MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_delivery_cursor.sql

SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='group_members'
             AND column_name='last_delivered_id');
SET @s := IF(@c = 0,
  'ALTER TABLE group_members ADD COLUMN last_delivered_id BIGINT UNSIGNED NOT NULL DEFAULT 0',
  'SELECT 1');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
