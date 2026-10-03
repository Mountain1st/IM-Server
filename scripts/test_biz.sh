#!/usr/bin/env bash
# 业务回归测试：启动服务器（MySQL 存储）后跑 scripts/biz_test.py
set -e
cd "$(dirname "$0")/.."
source scripts/db_env.sh

# IM_OFFLINE_PURGE_INTERVAL=2：离线 TTL 清理间隔压到 2 秒以便用例验证（生产默认 1800）
# IM_REGISTER_LIMIT_PER_WINDOW=100000：注册限流豁免（biz 需注册数十个测试账号；生产默认按 IP 3 次/10 分钟）
# IM_MEDIA_GC_INTERVAL_SEC=1：媒体 GC 轮询压到 1 秒（仅作用于超期行：PENDING>24h/孤儿>7d，
#   常规用例不受影响；回收 e2e 用 created_at 回拨 + 短等待验证）
# IM_SERVER_BIN 可指向 sanitizer 构建的服务端（如 build-asan/imserver）
# 导出给 biz_test.py 门禁用（GC e2e 需确认服务端确实按 1s 调度）
export IM_MEDIA_GC_INTERVAL_SEC=1
IM_SERVER_BIN="${IM_SERVER_BIN:-./build/imserver}"
IM_OFFLINE_PURGE_INTERVAL=2 IM_REGISTER_LIMIT_PER_WINDOW=100000 "$IM_SERVER_BIN" 9191 </dev/null >server.log 2>&1 &
SRV=$!

cleanup() {
  kill $SRV 2>/dev/null
  # 优雅停机（看门狗 15s 兜底）后仍存活的进程强杀（ASan 构建的 LSan
  # atexit 曾出现卡死不退的顽固进程——2026-09-30 实测；生产为普通构建不受影响）
  ( sleep 18; kill -9 $SRV 2>/dev/null ) &
  # 测试账号清理由 biz_test.py 末尾精确执行（删除本次注册账号及关联消息）；
  # 此处仅兜底清理历史遗留的旧格式测试账号（字母前缀，现行规则下不可能再产生）
  MYSQL_PWD="$IM_MYSQL_PASSWORD" mysql -u "$IM_MYSQL_USER" -h "$IM_MYSQL_HOST" \
    -e "USE $IM_MYSQL_DB; DELETE FROM users WHERE account LIKE 'alice%' OR account LIKE 'bob%' OR account LIKE 'carol%' OR account LIKE 'sink%' OR account LIKE 'spam%' OR account LIKE 'chk%' OR account LIKE 'pk%' OR account LIKE 'rd%' OR account LIKE 'off%' OR account LIKE 'nf%' OR account LIKE 'doc%';" \
    || echo "[warn] 遗留账号清理失败（不影响本次测试结果，但建议排查）" >&2
}
trap cleanup EXIT
sleep 1
python3 scripts/biz_test.py 127.0.0.1 9191