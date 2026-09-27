#!/usr/bin/env python3
"""Origin/Host 握手校验集成测试。

用法: python3 scripts/test_origin.py [host] [port]
预期: 服务器以允许列表 "127.0.0.1:<port>,http://127.0.0.1:8080" 启动（见 test_origin.sh）。
覆盖: 无 Origin（Host 命中允许列表→101）/ 非允许 Origin（→403）/ 允许 Origin（→101）。
"""
import socket
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9292


def handshake(origin=None):
    s = socket.create_connection((HOST, PORT), timeout=5)
    req = (
        "GET / HTTP/1.1\r\nHost: %s:%d\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n" % (HOST, PORT)
    )
    if origin is not None:
        req += "Origin: %s\r\n" % origin
    req += "\r\n"
    s.sendall(req.encode())
    data = b""
    try:
        while b"\r\n\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    s.close()
    return data.split(b"\r\n")[0].decode(errors="replace")


# 1) 无 Origin，Host 在允许列表内 → 应握手成功
status = handshake(None)
print("no-origin host-allowed ->", status)
assert "101" in status, status

# 2) 非允许 Origin → 应 403 拒绝
status = handshake("http://evil.example.com")
print("disallowed origin ->", status)
assert "403" in status, status

# 3) 允许的 Origin（配置了 http://127.0.0.1:8080）→ 应握手成功
status = handshake("http://127.0.0.1:8080")
print("allowed origin ->", status)
assert "101" in status, status

print("\norigin tests OK")
