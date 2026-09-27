#!/usr/bin/env bash
# 业务回归测试：启动服务器（MySQL 存储）后跑 scripts/biz_test.py
set -e
cd "$(dirname "$0")/.."
source scripts/db_env.sh

# IM_OFFLINE_PURGE_INTERVAL=2：离线 TTL 清理间隔压到 2 秒以便用例验证（生产默认 1800）
# IM_SERVER_BIN 可指向 sanitizer 构建的服务端（如 build-asan/imserver）
IM_SERVER_BIN="${IM_SERVER_BIN:-./build/imserver}"
IM_OFFLINE_PURGE_INTERVAL=2 "$IM_SERVER_BIN" 9191 </dev/null >server.log 2>&1 &
SRV=$!

cleanup() {
  kill $SRV 2>/dev/null
  # 测试账号清理由 biz_test.py 末尾精确执行（删除本次注册账号及关联消息）；
  # 此处仅兜底清理历史遗留的旧格式测试账号（字母前缀，现行规则下不可能再产生）
  MYSQL_PWD="$IM_MYSQL_PASSWORD" mysql -u "$IM_MYSQL_USER" -h "$IM_MYSQL_HOST" \
    -e "USE $IM_MYSQL_DB; DELETE FROM users WHERE account LIKE 'alice%' OR account LIKE 'bob%' OR account LIKE 'carol%' OR account LIKE 'sink%' OR account LIKE 'spam%' OR account LIKE 'chk%' OR account LIKE 'pk%' OR account LIKE 'rd%' OR account LIKE 'off%' OR account LIKE 'nf%' OR account LIKE 'doc%';" \
    || echo "[warn] 遗留账号清理失败（不影响本次测试结果，但建议排查）" >&2
}
trap cleanup EXIT
sleep 1
python3 scripts/biz_test.py 127.0.0.1 9191