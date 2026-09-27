#!/usr/bin/env bash
set -e
cd "$(dirname "$0")/.."
source scripts/db_env.sh
./build/imserver 9191 </dev/null >server.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1
echo "=== 客户端输出 ==="
./build/imclient 127.0.0.1 9191
echo "=== 服务器日志 ==="
cat server.log
