#!/usr/bin/env python3
"""IM 服务器连接测试脚本。
用法: python3 scripts/test_connect.py [host] [port]
依赖: pip install websocket-client
"""
import sys
import websocket

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = sys.argv[2] if len(sys.argv) > 2 else "9191"
URL = f"ws://{HOST}:{PORT}/"


def main():
    print(f"[test] 连接 {URL}")
    ws = websocket.create_connection(URL, timeout=5)
    print("[test] 握手成功")

    ws.send("hello from python tester")
    reply = ws.recv()
    print(f"[test] 服务器回复: {reply}")

    ws.close()
    print("[test] 连接测试通过")


if __name__ == "__main__":
    main()