#!/usr/bin/env bash
# 数据库连接环境变量（供测试/启动脚本使用，可被外部环境变量覆盖）。
# 服务器必需 IM_MYSQL_USER / IM_MYSQL_PASSWORD / IM_MYSQL_DB，否则启动报错退出。
export IM_MYSQL_HOST="${IM_MYSQL_HOST:-127.0.0.1}"
export IM_MYSQL_PORT="${IM_MYSQL_PORT:-3306}"
export IM_MYSQL_USER="${IM_MYSQL_USER:-IMS_jack}"
# 本地私密配置（git-ignored，可选）：.env.local 里可写 export IM_MYSQL_PASSWORD=...
_env_local="$(dirname "$0")/../.env.local"
[ -f "$_env_local" ] && source "$_env_local"
export IM_MYSQL_PASSWORD="${IM_MYSQL_PASSWORD:-change-me}"
export IM_MYSQL_DB="${IM_MYSQL_DB:-IMS_reactor_db}"