#!/usr/bin/env bash
# 本地 CI：一键跑完整验证矩阵（等价 GitHub Actions，见 .github/workflows/ci.yml）
# 前置：MySQL 127.0.0.1:3306（IMS_jack/520010/IMS_reactor_db，见 scripts/db_env.sh）
#       Redis 127.0.0.1:6379（缺失时自动跳过 Redis 模式并给出提示）
# 用法: bash scripts/ci.sh           # 构建+单测+biz 内存+Redis
#       bash scripts/ci.sh --full    # 追加 ASan 构建与全链路（更慢）
set -e
cd "$(dirname "$0")/.."
source scripts/db_env.sh

echo "=== [1/5] 依赖检查 ==="
MYSQL_PWD="$IM_MYSQL_PASSWORD" mysql -u "$IM_MYSQL_USER" -h "$IM_MYSQL_HOST" \
  -e "USE $IM_MYSQL_DB; SELECT 1" >/dev/null 2>&1 \
  || { echo "FAIL: MySQL 不可达或库未初始化（先导入 db/schema.sql）"; exit 1; }
echo "MySQL OK"
REDIS_OK=0
redis-cli -p 6379 ping >/dev/null 2>&1 && { REDIS_OK=1; echo "Redis OK"; } \
  || echo "SKIP: Redis 6379 不可达，将跳过 Redis 模式"

echo "=== [2/5] 全新构建（build-ci） ==="
rm -rf build-ci
cmake -B build-ci -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -1
cmake --build build-ci -j"$(nproc)" 2>&1 | grep -E "error|Error" && exit 1 || true

echo "=== [3/5] 单元测试 ==="
ctest --test-dir build-ci --output-on-failure 2>&1 | tail -2

echo "=== [4/5] 业务回归（内存模式） ==="
IM_SERVER_BIN=./build-ci/imserver bash scripts/test_biz.sh 2>&1 | tail -1

if [ "$REDIS_OK" = "1" ]; then
  echo "=== [5/5] 业务回归（Redis 模式） ==="
  IM_REDIS_ENABLED=1 IM_REDIS_PORT=6379 IM_SERVER_BIN=./build-ci/imserver \
    bash scripts/test_biz.sh 2>&1 | tail -1
else
  echo "=== [5/5] 业务回归（Redis 模式）：SKIP ==="
fi

if [ "$1" = "--full" ]; then
  echo "=== [+] Sanitizer（ASan+UBSan）构建与全链路 ==="
  rm -rf build-ci-asan
  cmake -B build-ci-asan -DIM_SANITIZE=ON 2>&1 | tail -1
  cmake --build build-ci-asan -j"$(nproc)" 2>&1 | grep -E " error" && exit 1 || true
  ctest --test-dir build-ci-asan --output-on-failure 2>&1 | tail -2
  IM_SERVER_BIN=./build-ci-asan/imserver ASAN_OPTIONS=detect_leaks=1 \
    bash scripts/test_biz.sh 2>&1 | tail -1
  grep -cE 'AddressSanitizer|runtime error' server.log \
    && { echo "FAIL: Sanitizer 有报告"; exit 1; } || echo "Sanitizer 零报告"
fi

rm -rf build-ci build-ci-asan
echo "=== CI 通过 ==="
