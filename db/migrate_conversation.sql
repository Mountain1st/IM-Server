-- 迁移：会话列表（conversation.list v3.2）——messages 增反向聚合索引
-- 背景：单聊会话聚合按「我收」方向 GROUP BY from_user；现有 idx_pair_ts
-- (from_user,to_user,ts) 只覆盖「我发」方向，补 (to_user,from_user,ts) 反向索引。
-- 纯索引变更，无数据迁移；幂等（information_schema 判定后加，可重复执行）。
-- 用法：MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_conversation.sql

SET @c := (SELECT COUNT(DISTINCT index_name) FROM information_schema.statistics
           WHERE table_schema=DATABASE() AND table_name='messages' AND index_name='idx_recv_peer');
SET @s := IF(@c = 0,
  'ALTER TABLE messages ADD INDEX idx_recv_peer (to_user, from_user, ts)',
  'SELECT 1');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
