-- 存量用户补齐"自己"联系人（幂等，可重复执行）
-- 用法: MYSQL_PWD='<密码>' mysql -u IMS_jack -h 127.0.0.1 IMS_reactor_db < db/migrate_self_contact.sql
-- 说明: 新用户注册时由服务器自动插入；本脚本仅为存量用户一次性补齐。
INSERT IGNORE INTO contacts (user_id, contact_id)
SELECT id, id FROM users;