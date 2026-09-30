-- 迁移：messages.message_id 普通索引 → 唯一索引
-- 背景（2026-09-30 自检）：MySQL 断线自愈的 INSERT 重试在"语句已执行但
-- 回执未达"时会重复插入同一 message_id（重复消息进离线补发/历史）。
-- 加唯一键后重试撞键被拒（errno 1062 非连接类错误，不再重试），
-- 客户端收失败回执后重试/离线补发兜底——宁拒不重。
-- 幂等：先检查重复数据（有重复则中止并报告，需人工处理）；索引已是
-- UNIQUE 则跳过。
-- 用法：mysql -u root -p IMS_reactor_db < db/migrate_unique_message_id.sql

-- 1) 重复数据检查（存在则中止；正常业务下 message_id 全局唯一）
SET @dup = (SELECT COUNT(*) FROM (
  SELECT message_id FROM messages GROUP BY message_id HAVING COUNT(*) > 1
) t);
SELECT IF(@dup = 0,
  'OK: 无重复 message_id，可加唯一索引',
  CONCAT('ABORT: 存在 ', @dup, ' 组重复 message_id，请人工去重后重跑')
) AS check_result;
SET @sql = IF(@dup = 0,
  IF((SELECT NON_UNIQUE FROM information_schema.STATISTICS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'messages'
        AND INDEX_NAME = 'idx_message_id' LIMIT 1) = 0,
    'SELECT ''SKIP: idx_message_id 已是唯一索引'' AS migrate_result',
    'ALTER TABLE messages DROP INDEX idx_message_id,
       ADD UNIQUE KEY idx_message_id (message_id)'),
  'SELECT ''中止：存在重复数据'' AS migrate_result');
PREPARE s FROM @sql;
EXECUTE s;
DEALLOCATE PREPARE s;

-- 2) 验证：重复插入必须被拒
-- （手动验证示例，不在本脚本内执行）
-- INSERT INTO messages (message_id, from_user, to_user, content, ts,
--   client_msg_id, chat_type, group_id, status)
--   SELECT message_id, from_user, to_user, 'dup-probe', ts, '', 'single', 0, 0
--   FROM messages LIMIT 1;  -- 预期 ERROR 1062 Duplicate entry
