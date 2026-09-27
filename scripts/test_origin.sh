#!/usr/bin/env bash
# Origin/Host 握手校验集成测试：以允许列表启动服务器后跑 scripts/test_origin.py
set -e
cd "$(dirname "$0")/.."
source scripts/db_env.sh

./build/imserver 9292 3 5 0 "127.0.0.1:9292,http://127.0.0.1:8080" \
  </dev/null >server_origin.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1
python3 scripts/test_origin.py 127.0.0.1 9292