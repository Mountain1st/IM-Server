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

## 修复记录（2026-10-01 审查整改，最小改动不碰源码）
- 已删除本文件原 1-33 行无关 schema 段落（groups 加 4 列/c9ad2d3），仅保留 emoji 验证结论。
- 提交路径说明：本报告为过程记录，简报步骤5的计划文件已在 45d3761 入库，本次报告保留在工作区（已在 51ff1e0 入库则保留历史，不再新增 -f 提交，本次为普通跟踪修改提交）。
- 复测证据（只读确认）：`ctest --test-dir build 2>&1 | tail -3` → `100% tests passed, 0 tests failed out of 9` / `Total Test time (real) = 0.80 sec`，9/9 通过。
