# Nginx 接入部署指南

Nginx 定位：**TLS 终结（wss://）+ WebSocket 反向代理 + 入口连接限流 + 传递真实客户端 IP**。
服务端协议层零改动，仅启动参数/环境变量配合。

## 1. 拓扑

```text
客户端 ── wss://im.example.com（443, TLS 终结）
            │
        Nginx（nginx/im.conf）
            │  proxy_pass → imserver:9191
        imserver（裸 WebSocket）
```

## 2. Docker Compose（默认，开箱即用）

`docker-compose.yml` 已含 nginx 服务（`nginx:1.27-alpine`，挂载 `nginx/im.conf`）：

```bash
docker compose up -d --build      # 起 mysql + redis + imserver + nginx
# 客户端连 ws://<主机>:8080
```

`nginx/im.conf` 上游默认 `server imserver:9191`（同 compose 网络）。
**裸机同机部署**：注释该行，启用 `server 127.0.0.1:9191`。

## 3. 服务端配合（三个坑的处理）

| 项 | 处理 |
|---|---|
| 单 IP 限连误伤 | 反代后服务端看到的来源全是 Nginx，**全体用户共享额度**：设 `IM_MAX_PER_IP_CONN=0` 关闭，入口限流交给 Nginx `limit_conn perip 50`（conf 已含） |
| 真实 IP | `IM_TRUSTED_PROXIES=<Nginx 的 IP>`（逗号分隔）。对端在列表内时握手日志显示 `X-Forwarded-For` 真实客户端 IP；**未配置则忽略该头（防客户端伪造）**；头值含非 IP 字符整体丢弃（防日志注入） |
| Host 校验 | 需要时启动第 5 参数加白名单：`./imserver 9191 2 3 500 im.example.com` |
| 超时 | conf 已设 `proxy_read_timeout 90s`（服务端心跳 30s/僵尸 60s，留余量） |

## 4. TLS（wss://，公网正式）

`nginx/im.conf` 方案 B 段（注释态）：置备证书后放开，客户端从 `ws://host:8080` 换
`wss://im.example.com`，协议层零变化。

## 5. 验证清单

1. `nginx -t`（容器内 `docker compose exec nginx nginx -t`）
2. 直连回归不受影响：`bash scripts/test_biz.sh`（连 9191）
3. **过代理全量回归**（已实测 207/207）：`python3 scripts/biz_test.py <主机> 8080`
4. 真实 IP 抽查：服务端日志 `WebSocket 握手成功 真实IP=x.x.x.x`
5. 空闲挂 5 分钟不掉线（无 499/504）

## 6. 已知边界

- Docker Desktop（WSL2）下 `--network host` 指向 Docker VM 网络而非 WSL，**不要用**；
  用端口映射 `-p 8080:8080` + upstream `host.docker.internal:9191`（实测可用）
- 服务端 per-IP 限连已移交 Nginx：`IM_MAX_PER_IP_CONN` 保持 0，勿双开
- XFF 只做显示（日志/`Session::realIp()`），不参与限流判定——如需按真实 IP 限流，
  由 Nginx `limit_conn` 承担
