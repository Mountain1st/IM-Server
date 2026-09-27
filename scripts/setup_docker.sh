#!/usr/bin/env bash
# 安装 Docker Engine + Compose 插件，并把当前用户加入 docker 组。
# 需要 sudo 权限（执行时输入密码）。用法: bash scripts/setup_docker.sh
set -euo pipefail

if command -v docker >/dev/null 2>&1; then
  echo "[setup] docker 已安装: $(docker --version)"
else
  echo "[setup] 安装 Docker Engine（官方脚本，Ubuntu 26.04）..."
  curl -fsSL https://get.docker.com | sudo sh
fi

echo "[setup] 配置 docker 组..."
sudo usermod -aG docker "$USER"
echo "[setup] 校验 docker compose 插件..."
docker compose version

echo
echo "完成。请重新打开终端（或执行: newgrp docker）使 docker 组生效，然后："
echo "  cd $(pwd)"
echo "  docker compose up -d --build"
echo "  curl -s http://127.0.0.1:9191/health"