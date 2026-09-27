#!/usr/bin/env bash
# 数据库连接环境变量（供测试/启动脚本使用，可被外部环境变量覆盖）。
# 服务器必需 IM_MYSQL_USER / IM_MYSQL_PASSWORD / IM_MYSQL_DB，否则启动报错退出。
export IM_MYSQL_HOST="${IM_MYSQL_HOST:-127.0.0.1}"
export IM_MYSQL_PORT="${IM_MYSQL_PORT:-3306}"
export IM_MYSQL_USER="${IM_MYSQL_USER:-IMS_jack}"
export IM_MYSQL_PASSWORD="${IM_MYSQL_PASSWORD:-520010}"
export IM_MYSQL_DB="${IM_MYSQL_DB:-IMS_reactor_db}"