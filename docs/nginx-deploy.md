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
cp .env.example .env              # 必填 IM_MYSQL_PASSWORD / IM_S3_ACCESS_KEY /
                                  # IM_S3_SECRET_KEY；公网部署补 IM_S3_PUBLIC_ENDPOINT
# 编辑 .env 填强密码（openssl rand -base64 24）
docker compose up -d --build      # 起 mysql + redis + minio + imserver + nginx
# 客户端连 ws://<主机>:8080（公网正式须启 wss，见 §4）
```

**凭据强校验（任务⑤）**：`docker-compose.yml` 对三项必需变量用 `${VAR:?msg}`
语法——未在 `.env` 提供时 compose 直接报错退出，不再以 `change-me`/`minioadmin`
弱默认启动。

`nginx/im.conf` 上游默认 `server imserver:9191`（同 compose 网络）。
**裸机同机部署**：注释该行，启用 `server 127.0.0.1:9191`。

## 3. 服务端配合（三个坑的处理）

| 项 | 处理 |
|---|---|
| 单 IP 限连误伤 | 反代后服务端看到的来源全是 Nginx，**全体用户共享额度**：设 `IM_MAX_PER_IP_CONN=0` 关闭，入口限流交给 Nginx `limit_conn perip 50`（conf 已含） |
| 真实 IP | `IM_TRUSTED_PROXIES=<Nginx 的 IP>`（逗号分隔；compose 部署标准已固化网桥 IP，**换机器/重建网络后用 `docker network inspect <网络名>` 核对 Gateway**）。对端在列表内时握手日志显示 `X-Forwarded-For` 真实客户端 IP；**未配置则忽略该头（防客户端伪造）**；头值含非 IP 字符整体丢弃（防日志注入） |
| Host 校验 | 需要时启动第 5 参数加白名单：`./imserver 9191 2 3 50 im.example.com` |
| 超时 | conf 已设 `proxy_read_timeout 90s`（服务端心跳 30s/僵尸 60s，留余量） |

## 4. TLS（wss://，公网正式）

`nginx/im.conf` 方案 B 段（注释态，已含 TLS 1.2/1.3 套件与会话缓存基线）：
置备证书后放开，客户端从 `ws://host:8080` 换 `wss://im.example.com`，协议层零变化。

启用步骤：

```bash
# 1. 证书放入 nginx/certs/（compose 已挂载 /etc/nginx/certs:ro），按域名命名
mkdir -p nginx/certs
# 上传 im.example.com.pem / im.example.com.key（或 certbot 签发）

# 2. 取消 nginx/im.conf 方案 B 两段注释，替换 im.example.com 为实际域名
# 3. 重载 nginx（compose：docker compose restart nginx）
# 4. 验证：curl -sS https://im.example.com/health（或 wss 客户端连接）
# 5. 全量回归过代理：python3 scripts/biz_test.py <域名> 443
```

生产建议：先保留 8080 作过渡，确认 wss 全量可用后再关闭 8080 与启用 80→443 跳转；
HSTS 待全站 HTTPS 稳定后再打开（conf 中已备注释行）。

### 4.1 对象存储公网地址（与 TLS 独立，但公网部署必配）

预签名上传/下载 URL 的 Host 会写入 SigV4 签名，必须让**客户端**能解析：

```text
客户端 ── media.presign ──> imserver（内网签，host=公网域名）
客户端 ── HTTPS PUT ──────> OSS/CDN 公网域名（不经 imserver，数据面）
```

`.env` 配置 `IM_S3_PUBLIC_ENDPOINT=<域名>`（`IM_S3_PUBLIC_SCHEME=https`）。
不配时回落内网 `minio:9000` —— 外部客户端**无法上传/下载**（仅同网段联调用）。
阿里云 OSS 示例：`your-bucket.oss-cn-hangzhou.aliyuncs.com`（同时把
`IM_S3_ENDPOINT` 指向 OSS 内网/公网 endpoint、`IM_S3_PATH_STYLE=0` 视 OSS 要求）。

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
