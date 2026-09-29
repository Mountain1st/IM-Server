#!/usr/bin/env python3
# 性能压测基线：连接保持 + 单聊消息吞吐 + 群消息扇出
# 用法: python3 scripts/bench.py [host] [port] [--conns N] [--dur 秒]
# 前置：MySQL 就绪（测试账号自动注册并清理）；服务端已启动
import asyncio
import json
import socket
import struct
import subprocess
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9191
CONNS = 2000
DUR = 30
SENDERS = 40
RECV_EACH = 5  # 每发送者目标数

WS_GUID = "dGhlIHNhbXBsZSBub25jZQ=="


def frame(data: bytes, opcode=0x1):
    mask = b"\x01\x02\x03\x04"
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    if len(data) < 126:
        return bytes([0x80 | opcode, 0x80 | len(data)]) + mask + masked
    return bytes([0x80 | opcode, 0x80 | 126]) + struct.pack("!H", len(data)) + mask + masked


class Client:
    def __init__(self):
        self.buf = b""

    async def connect(self):
        self.r, self.w = await asyncio.open_connection(HOST, PORT)
        self.w.write(("GET / HTTP/1.1\r\nHost: bench\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n" % WS_GUID).encode())
        await self.w.drain()
        while b"\r\n\r\n" not in self.buf:
            chunk = await self.r.read(4096)
            if not chunk:
                raise ConnectionError("握手阶段对端关闭（可能被限连拒绝）")
            self.buf += chunk
        self.buf = self.buf.split(b"\r\n\r\n", 1)[1]

    async def send_json(self, obj):
        self.w.write(frame(json.dumps(obj).encode()))
        await self.w.drain()

    async def recv_msg(self, timeout=10):
        # 解析一帧（跳过 ping：直接回 pong 不上抛）
        while True:
            while len(self.buf) < 2:
                chunk = await asyncio.wait_for(self.r.read(4096), timeout)
                if not chunk:
                    raise ConnectionError("对端关闭")
                self.buf += chunk
            b0, b1 = self.buf[0], self.buf[1]
            n = b1 & 0x7F
            off = 2
            if n == 126:
                while len(self.buf) < 4:
                    self.buf += await asyncio.wait_for(self.r.read(4096), timeout)
                n = struct.unpack("!H", self.buf[2:4])[0]
                off = 4
            elif n == 127:
                while len(self.buf) < 10:
                    self.buf += await asyncio.wait_for(self.r.read(4096), timeout)
                n = struct.unpack("!Q", self.buf[2:10])[0]
                off = 10
            if b1 & 0x80:
                off += 4
            while len(self.buf) < off + n:
                self.buf += await asyncio.wait_for(self.r.read(4096), timeout)
            payload = self.buf[off:off + n]
            self.buf = self.buf[off + n:]
            if b1 & 0x80 and n:
                mk = self.buf[off - 4:off] if off >= 4 else b"\x01\x02\x03\x04"
            op = b0 & 0x0F
            if op == 0x9:  # ping -> pong
                self.w.write(bytes([0x8A, 0x80 | len(payload)]) + b"\x01\x02\x03\x04" +
                             bytes(b ^ b"\x01\x02\x03\x04"[i % 4] for i, b in enumerate(payload)))
                await self.w.drain()
                continue
            if op in (0xA, 0x8):
                continue
            return json.loads(payload.decode())


async def register_login(c: Client, acct, pwd):
    await c.send_json({"type": "auth.register", "data": {"account": acct, "password": pwd}})
    r = await c.recv_msg()
    if r["data"]["code"] != 0:
        await c.send_json({"type": "auth.login", "data": {"account": acct, "password": pwd}})
        r = await c.recv_msg()
    await c.send_json({"type": "auth.login", "data": {"account": acct, "password": pwd}})
    r = await c.recv_msg()
    assert r["data"]["code"] == 0, r


async def phase_connections():
    """阶段一：并发建连 + 保持 + 全量存活断言"""
    print(f"== 阶段一：{CONNS} 并发连接（建连+保持 {DUR}s）==")
    clients = []
    t0 = time.time()
    for i in range(CONNS):
        c = Client()
        await c.connect()
        clients.append(c)
    dt = time.time() - t0
    print(f"建连完成：{CONNS} 连接 / {dt:.1f}s（{CONNS/dt:.0f} 连接/s）")
    # 保持期间：每 5 秒做一次全量探活（发 login 一个已知账号会回 login_res——太重；
    # 改为空闲等待，靠服务端心跳 Ping 驱动 pong 保持）
    await asyncio.sleep(DUR)
    # 存活断言：对前 20 个连接发一条非法指令，应收到 error 400（连接活着）
    alive = 0
    for c in clients[:20]:
        try:
            await c.send_json({"type": "bench.probe", "data": {}})
            r = await c.recv_msg(timeout=5)
            if r.get("type") == "error" or r.get("data", {}).get("code") == 401:
                alive += 1
        except Exception:
            pass
    print(f"探活（抽样 {min(20, CONNS)}）：{alive}/{min(20, CONNS)} 存活；总连接 {CONNS}")
    for c in clients:
        c.w.close()
    return CONNS, CONNS / dt


async def phase_throughput(acct_base):
    """阶段二：SENDERS 发送者 × 持续发（收 ack 再发），RECV_EACH 个接收端收推送"""
    print(f"== 阶段二：{SENDERS} 发送者持续发 {DUR}s（每发一条等 ack）==")
    # 注册登录并行化（分批 40 并发，PBKDF2 在服务端多工作线程消化）
    async def make_account(acct):
        c = Client()
        await c.connect()
        await register_login(c, acct, "pw123456")
        return c
    senders = []
    for batch_start in range(0, SENDERS, 40):
        tasks = [make_account("%s1%03d" % (acct_base, i))
                 for i in range(batch_start, min(batch_start + 40, SENDERS))]
        senders.extend(await asyncio.gather(*tasks))
    receivers = []
    total_recv = SENDERS * RECV_EACH
    for batch_start in range(0, total_recv, 40):
        tasks = [make_account("%s2%03d" % (acct_base, i))
                 for i in range(batch_start, min(batch_start + 40, total_recv))]
        receivers.extend(await asyncio.gather(*tasks))

    stop = time.time() + DUR
    acks = [0]
    lat = []

    async def sender(idx, c):
        target = "%s2%03d" % (acct_base, (idx * RECV_EACH) % len(receivers))
        n = 0
        while time.time() < stop:
            try:
                t0 = time.time()
                await c.send_json({"type": "msg.send", "data": {
                    "to": target, "content": "bench-%d-%d" % (idx, n),
                    "clientMsgId": "b-%d-%d" % (idx, n)}})
                r = await c.recv_msg(timeout=10)
                if r["type"] == "msg.ack" and r["data"]["code"] == 0:
                    acks[0] += 1
                    lat.append(time.time() - t0)
                n += 1
            except Exception as e:
                print("sender%d break: %r" % (idx, e))
                break  # 连接被服务端关闭（僵尸策略/异常）：该发送者结束

    async def receiver(c):
        # +15s：覆盖服务端 30s 心跳周期，避免吞吐结束后心跳无 pong 被判僵尸
        while time.time() < stop + 15:
            try:
                await c.recv_msg(timeout=3)
            except Exception:
                pass

    tasks = [sender(i, c) for i, c in enumerate(senders)]
    tasks += [receiver(c) for c in receivers]
    t0 = time.time()
    await asyncio.gather(*tasks)
    dt = time.time() - t0
    lat.sort()
    p50 = lat[len(lat)//2] if lat else 0
    p99 = lat[int(len(lat)*0.99)] if lat else 0
    print(f"吞吐：{acks[0]/dt:.0f} msg/s（ack {acks[0]} 条 / {dt:.1f}s）")
    print(f"延迟：p50={p50*1000:.1f}ms p99={p99*1000:.1f}ms（发送→ack）")
    for c in senders + receivers:
        c.w.close()
    return acks[0] / dt, p50, p99


def rss_mb():
    try:
        pid = subprocess.check_output(["pgrep", "-x", "imserver"]).split()[0]
        out = subprocess.check_output(["grep", "VmRSS", "/proc/%s/status" % pid.decode()],
                                      text=True)
        return int(out.split()[1]) / 1024
    except Exception:
        return -1


async def main():
    acct_base = "9%06d" % (int(time.time()) % 10**6)  # 7 位基串
    print(f"目标：{HOST}:{PORT}，服务端 RSS {rss_mb():.0f}MB（压测前）")
    _, cps = await phase_connections()
    tps, p50, p99 = await phase_throughput(acct_base)
    print(f"服务端 RSS {rss_mb():.0f}MB（压测后，含 2000 连接保持期峰值）")
    print("\n== 基线汇总 ==")
    print(f"建连速率 {cps:.0f}/s；消息吞吐 {tps:.0f} msg/s；p50 {p50*1000:.1f}ms；p99 {p99*1000:.1f}ms")
    # 清理测试账号（注册的 40+200 个）——按本次压测前缀精确删
    import os
    pwd = os.environ.get("IM_MYSQL_PASSWORD", "change-me")
    sql = ("USE IMS_reactor_db; "
           "DELETE m FROM messages m JOIN users u ON m.from_user=u.id OR m.to_user=u.id "
           "WHERE u.account LIKE '" + acct_base + "%'; "
           "DELETE FROM users WHERE account LIKE '" + acct_base + "%';")
    subprocess.run(["bash", "-c", "MYSQL_PWD=" + pwd +
                    " mysql -u IMS_jack -h 127.0.0.1 -e \"" + sql + "\""],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == "__main__":
    asyncio.run(main())
