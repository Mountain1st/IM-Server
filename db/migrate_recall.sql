-- 迁移：消息撤回（msg.recall）——messages 增 recalled_ts 列
-- 背景：status 扩展终态 3=RECALLED；recalled_ts 记录撤回时间（审计/排障）。
-- 历史数据 recalled_ts 默认 0，保持回读兼容。
-- 幂等：查询 information_schema.COLUMNS 判定后加列，可重复执行。
-- 用法：MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_recall.sql

SET @c := (SELECT COUNT(*) FROM information_schema.columns
           WHERE table_schema=DATABASE() AND table_name='messages' AND column_name='recalled_ts');
SET @s := IF(@c = 0,
  'ALTER TABLE messages ADD COLUMN recalled_ts BIGINT NOT NULL DEFAULT 0',
  'SELECT 1');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
