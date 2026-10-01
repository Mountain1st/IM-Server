-- 迁移：messages 加 msg_type 列（图片消息存储层，任务 2）
-- 背景：text/image 双 msg_type 落库，content 存文本或 /uploads/<file> 路径；
-- 历史数据默认为 text，保持回读兼容。
-- 幂等：msg_type 列已存在则跳过（查询 information_schema.COLUMNS 判定）。
-- 用法：mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_msg_type.sql

-- 1) 列存在性检查（存在则跳过，不存在则加列）
SET @col_exists = (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'messages'
    AND COLUMN_NAME = 'msg_type');
SELECT IF(@col_exists = 0,
  'OK: msg_type 列不存在，准备加列',
  'SKIP: msg_type 列已存在'
) AS check_result;
SET @sql = IF(@col_exists = 0,
  'ALTER TABLE messages ADD COLUMN msg_type VARCHAR(16) NOT NULL DEFAULT ''text''',
  'SELECT ''SKIP: msg_type 列已存在'' AS migrate_result');
PREPARE s FROM @sql;
EXECUTE s;
DEALLOCATE PREPARE s;

-- 2) 验证：列定义符合预期（VARCHAR(16) / NOT NULL / 默认 text）
-- （只读查询，手动确认输出即可）
SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT
  FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'messages'
    AND COLUMN_NAME = 'msg_type';
