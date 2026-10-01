# 任务 1 报告：DB schema（groups 加 4 列）
## 状态
DONE

## 实现内容
- 文件：`db/schema.sql`（`` `groups` `` 表，行 69-79），新增 4 列，与简报逐字一致：
  - `announcement TEXT NOT NULL` — 群公告正文（单条最新，空=无公告）
  - `announce_ts BIGINT NOT NULL DEFAULT 0` — 公告发布时间
  - `dissolved TINYINT NOT NULL DEFAULT 0` — 0=正常，1=已解散
  - `dissolved_ts BIGINT NOT NULL DEFAULT 0` — 解散时间
- 最小改动：仅 +4 行，其余表/索引未动。中文注释、英文标识符符合规范。
- 提交：`c9ad2d3 feat: groups表加公告/解散四列`（与简报指定 message 一致）。

## 验证结果
环境说明：127.0.0.1:3306 起初无 MySQL 服务（ERROR 2003），按任务说明用本地临时库验证：
`mysqld --no-defaults --datadir=/tmp/im-mysql --port=3306`，建用户 IMS_jack/520010 后导入。

1. 导入 + DESC（简报步骤 2 等价命令）：
   - `MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 < db/schema.sql` → 无报错，IMPORT_OK
   - `DESC groups` 输出含全部 8 列：group_id/name/owner_id/created_ts/announcement/announce_ts/dissolved/dissolved_ts
2. 幂等：重复导入一次 → REIMPORT_OK，无报错（IF NOT EXISTS 生效）。
3. INFORMATION_SCHEMA.COLUMNS 确认 4 列类型/默认值符合预期（announce_ts/dissolved/dissolved_ts 默认 0，announcement TEXT 无默认值）。
4. 老库补列路径验证（临时库模拟旧表）：在 `im_old_test.groups`（旧 4 列结构）上执行
   `ALTER TABLE ... ADD COLUMN announcement TEXT NOT NULL, ADD announce_ts ..., ADD dissolved ..., ADD dissolved_ts ...` → 成功，DESC 与新 schema 一致；测试库已 DROP。真实开发库若已存在旧 `groups` 表，需执行同等 ALTER（禁 TRUNCATE/DROP）。
5. 库身份：临时 fresh 库 `users` 为空（无 id 1/2 真实用户），属预期——本次未触碰任何用户数据。

## 修改文件
- `db/schema.sql`（+4 行）

## 自审
- diff 仅 4 行新增，与简报 SQL 逐字一致（含注释与默认值），无范围外改动。
- `git status` 确认仅 `db/schema.sql` 进入提交，其余 untracked 文件未动。
- 疑虑：无。唯一环境差异是 3306 原无服务、改用临时本地库验证；导入/DESC/幂等/ALTER 四项均通过，开发库上线时若表已存在记得手动 ALTER 补列。

---

# 任务 1 报告（2026-10-01 图片消息计划）：P0 emoji 链路验证

## 状态
DONE（零代码改动，结论符合简报“零改动预期”）

## 改动文件
- 无源码改动。仅本报告追加结论。
- 临时验证脚本 `/tmp/emoji_check.py`（19 项断言，仓库外，不入库不提交）。

## 验证过程（TDD：先写验证脚本，首轮暴露 3 处脚本自身协议假设错误并修正，与服务端无关）
- 首轮 FAIL 均为脚本断言与实际协议不符（服务端回 `msg.recv` 而非 `msg.new`；`chat.history_res` 字段为 `msgs`；`group.announce` 后有 `group.announced` 推送需先消费，且推送只带 `groupId`），内容字节本身全对；修正脚本后 19/19 全绿。

## 跑的命令与输出
1. `python3 /tmp/emoji_check.py 127.0.0.1 9193`（临时服务 `./build/imserver 9193`，MySQL 真库）
   - 输出：`19 tests, 0 failures`，覆盖：昵称 `昵称🎉` 注册回显、单聊 `content=hi 👋` + `clientMsgId=clientMsgId-🙂-001` 收发一致、`chat.history` 落库回读一致、群公告 `公告📢🎉` 经 `group.list` 回读一致、DB 原始字节四字段一致、无 U+FFFD 替换、`HEX(content)` 与 UTF-8 编码逐字节一致、精确清理后 `COUNT(*)=0`。
   - 链路字符集依据：`db/schema.sql` 全表 `CHARSET=utf8mb4`，`src/mysql_client.cpp:70,125` 连接字符集 `utf8mb4`。
2. `bash scripts/test_biz.sh 2>&1 | tail -5`（内存模式）
   - 输出：`246 tests, 0 failures`（`grep -c ^PASS` = 246，无 FAIL 行）。
3. `IM_REDIS_ENABLED=1 IM_REDIS_PORT=6379 bash scripts/test_biz.sh 2>&1 | tail -5`（Redis 模式，本地 6379）
   - 输出：`246 tests, 0 failures`（`grep -c ^PASS` = 246，无 FAIL 行）。
   - 注：计划/简报写 210 为旧数，以实际为准，当前基线为 246。
4. `ctest --test-dir build 2>&1 | tail -5`
   - 输出：`100% tests passed, 0 tests failed out of 9`。

## 测试证据
- `/tmp/biz_mem.log` 尾行 `246 tests, 0 failures`；`/tmp/biz_redis.log` 尾行 `246 tests, 0 failures`；ctest 9/9。
- 数据安全：真实用户 id1/2（JackX/善出奇者）保留；`SELECT COUNT(*) FROM users WHERE account REGEXP '^9[0-9]{10}$'` = 0，无本次残留；未用 TRUNCATE。

## 自审
- `git status` 干净，无源码/配置改动，无范围外改动。
- 疑虑：无。唯一环境点：`scripts/db_env.sh` 用 `$0` 定位 `.env.local`，在交互 shell 直接 `source` 时读不到密码（值为 `change-me` 导致 1045）；经 `bash scripts/test_biz.sh` 路径运行则正常，本次 biz 双模式均走后者，不影响结论。
- 提交说明：本目录被 `.superpowers/sdd/.gitignore`（内容 `*`）忽略、且历史报告均未入库；按任务指令“必须 commit”对本文件单文件 `git add -f` 入库，无其他文件变动。
