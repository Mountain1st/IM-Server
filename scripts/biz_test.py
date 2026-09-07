#!/usr/bin/env python3
"""IM 服务器业务回归测试。

用法: python3 scripts/biz_test.py [host] [port]
无第三方依赖（直接用 socket 实现 WebSocket 客户端）。
覆盖: 注册/重复注册/登录/错误密码/登录限流/消息收发/404/400/401/登出/离线补发/消息发送限流。
"""
import base64
import json
import os
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9191

results = []


def check(name, cond):
    results.append((name, cond))
    print(("PASS " if cond else "FAIL ") + name)


class WS:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET / HTTP/1.1\r\nHost: {HOST}:{PORT}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.s.sendall(req.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            data += self.s.recv(4096)
        assert b"101" in data.split(b"\r\n")[0], data

    def send(self, payload):
        data = payload.encode()
        self._send_frame(0x1, data)

    def _send_frame(self, opcode, data, masked=True):
        n = len(data)
        hdr = bytes([0x80 | opcode, (0x80 if masked else 0) | (n if n < 126 else (126 if n <= 0xFFFF else 127))])
        if n >= 126 and n <= 0xFFFF:
            hdr += struct.pack("!H", n)
        elif n > 0xFFFF:
            hdr += struct.pack("!Q", n)
        if masked:
            mask = os.urandom(4)
            hdr += mask
            hdr += bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        else:
            hdr += data
        self.s.sendall(hdr)

    def recv(self, timeout=5):
        # 循环跳过控制帧：服务器每 30s 发 Ping（须回 Pong 并继续等数据帧）
        self.s.settimeout(timeout)
        while True:
            h = self.s.recv(2)
            if len(h) < 2:
                return None
            b0, b1 = h[0], h[1]
            opcode = b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self.s.recv(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self.s.recv(8))[0]
            if b1 & 0x80:
                mask = self.s.recv(4)
                data = self.s.recv(n)
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            else:
                data = self.s.recv(n)
            if opcode == 0x9:          # Ping -> 回 Pong 后继续
                self._send_frame(0xA, data)
                continue
            if opcode == 0xA:          # Pong -> 跳过
                continue
            if opcode == 0x8:          # Close -> 视为断开
                return None
            return data.decode()

    def close(self):
        self.s.close()


def env(t, d):
    return json.dumps({"type": t, "data": d})


def register(ws, u, p, nick=None):
    # u 为登录账号 account；nick 为昵称（缺省=账号）
    data = {"account": u, "password": p}
    if nick is not None:
        data["username"] = nick
    ws.send(env("auth.register", data))
    return json.loads(ws.recv())


def login(ws, u, p):
    ws.send(env("auth.login", {"account": u, "password": p}))
    return json.loads(ws.recv())


def send_msg(ws, to, content, cid):
    ws.send(env("msg.send", {"to": to, "content": content, "clientMsgId": cid}))
    return json.loads(ws.recv())


def main():
    ts = int(time.time())
    # 账号规则：10-11 位纯数字（如手机号）。用毫秒级时间戳构造，
    # 确保唯一且符合格式（len=13 超 11 位，截取后 11 位 + 尾号区分）。
    base = str(ts * 1000)[-9:]          # 9 位数字基串
    A = base + "0"                       # alice → 10 位
    B = base + "1"                       # bob   → 10 位
    C = base + "2"                       # carol → 10 位
    # v1.8 联系人/隐私测试用独立账号（与前面账号隔离开）
    G = base + "11"                      # gerald（隐私开关）
    H = base + "12"                      # henry（隐私开关）
    SINK = base + "3"
    SINK2 = base + "6"
    SPAM = base + "4"
    SPAM2 = base + "5"
    # 合规但必然不存在的 11 位账号（chat.create 404 用）
    NOBODY = base + "77"

    a = WS()
    b = WS()

    # 注册
    r = register(a, A, "pw1234", nick="小爱")
    check("register alice ok", r["type"] == "auth.register_res" and r["data"]["code"] == 0
          and r["data"]["data"]["username"] == "小爱"
          and r["data"]["data"]["account"] == A)
    alice = r["data"]["data"]["userId"]

    r = register(b, B, "pw1234", nick="小波")
    check("register bob ok", r["type"] == "auth.register_res" and r["data"]["code"] == 0
          and r["data"]["data"]["username"] == "小波")
    bob = r["data"]["data"]["userId"]

    # 账号规则：非 10-11 位数字一律 400（错误消息为专用中文原因）
    acct_tests = [
        ("too_short_9", "123456789"),            # 9 位过短
        ("too_long_12", "123456789012"),         # 12 位过长
        ("with_letters", base + "a"),            # 含字母
        ("single_zero", "0"),                    # 输入 "0" → 400（不再误报 404）
        ("empty", ""),                           # 空
    ]
    for i, (label, acct) in enumerate(acct_tests):
        a.send(env("auth.register", {"account": acct, "password": "pw1234"}))
        r = json.loads(a.recv())
        check("register account rule %d (%s) 400" % (i + 1, label),
              r["type"] == "error" and r["data"]["code"] == 400)
    # chat.create 同规则：格式不合规 400（须在登录后测，否则先被 401 拦截）
    # 移至下方登录段后执行

    # 重复注册（同账号、合法密码 → 409）
    r = register(b, B, "pw1234")
    check("dup register 409", r["type"] == "auth.register_res" and r["data"]["code"] == 409)

    # 空参数（密码空 → 参数错误）
    a.send(env("auth.register", {"account": A + "6", "password": ""}))
    r = json.loads(a.recv())
    check("register bad params 400", r["type"] == "error" and r["data"]["code"] == 400)

    # 密码规则：6-32 位字母数字组合（须含字母与数字）。
    # 账号用合规 10-11 位数字（在合规账号上专测密码规则）
    pwd_tests = [
        "ab1",                 # 过短
        "a1" * 17,             # 34 位过长
        "abc12#",              # 含非法字符
        "abcdef",              # 仅字母
        "123456",              # 仅数字
    ]
    for i, pwd in enumerate(pwd_tests):
        a.send(env("auth.register", {"account": base + str(10 + i), "password": pwd}))
        r = json.loads(a.recv())
        check("register pwd rule %d 400" % (i + 1),
              r["type"] == "error" and r["data"]["code"] == 400)

    # 昵称 ≥32 位拒绝
    a.send(env("auth.register", {"account": base + "15", "password": "pw1234",
                                 "username": "昵" * 32}))
    r = json.loads(a.recv())
    check("register nickname >=32 400", r["type"] == "error" and r["data"]["code"] == 400)

    # 登录
    r = login(a, A, "pw1234")
    check("login alice ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)
    alice_token = r["data"]["data"]["token"]
    r = login(b, B, "pw1234")
    check("login bob ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)
    bob_token = r["data"]["data"]["token"]

    # 错误密码
    r = login(a, A, "wrong")
    check("wrong password 403", r["type"] == "auth.login_res" and r["data"]["code"] == 403)

    # 登录限流：连续失败 5 次后第 6 次返回 429
    c = WS()
    for i in range(5):
        r = login(c, A, "wrong")
        assert r["data"]["code"] in (403, 429), r
    r = login(c, A, "pw1234")
    check("login rate limit 429", r["data"]["code"] == 429)
    c.close()

    # auth.token 免密重连
    d = WS()
    d.send(env("auth.token", {"token": alice_token}))
    r = json.loads(d.recv())
    check("token reconnect ok", r["type"] == "auth.token_res"
          and r["data"]["code"] == 0 and r["data"]["data"]["userId"] == alice)
    # 重连后可正常收发
    r = send_msg(d, bob, "via token", "tok1")
    check("token session send ok", r["data"]["code"] == 0)
    json.loads(b.recv())
    # 无效 token
    d.send(env("auth.token", {"token": "bogus"}))
    r = json.loads(d.recv())
    check("token reconnect invalid 401", r["type"] == "auth.token_res"
          and r["data"]["code"] == 401)
    d.close()

    # 消息收发
    r = send_msg(a, bob, "hello bob", "m1")
    check("alice ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0
          and r["data"]["clientMsgId"] == "m1")
    r = json.loads(b.recv())
    check("bob recv ok", r["type"] == "msg.recv" and r["data"]["content"] == "hello bob"
          and r["data"]["from"] == A and r["data"]["fromNick"] == "小爱")

    # 新建对话 chat.create：按 account 查找，存在返回 account+昵称；不存在 404；未登录 401
    # 账号规则：格式不合规（"0"/5位）→ 400（登录后测，专测格式而非 401）
    a.send(env("chat.create", {"account": "0"}))
    r = json.loads(a.recv())
    check("chat.create account rule (0) 400", r["type"] == "chat.create_res"
          and r["data"]["code"] == 400)
    a.send(env("chat.create", {"account": "12345"}))
    r = json.loads(a.recv())
    check("chat.create account rule (5digits) 400", r["type"] == "chat.create_res"
          and r["data"]["code"] == 400)
    a.send(env("chat.create", {"account": B}))
    r = json.loads(a.recv())
    check("chat.create exists", r["type"] == "chat.create_res" and r["data"]["code"] == 0
          and r["data"]["data"]["userId"] == bob
          and r["data"]["data"]["account"] == B
          and r["data"]["data"]["username"] == "小波")
    # 合规 11 位数字但不存在 → 404
    a.send(env("chat.create", {"account": NOBODY}))
    r = json.loads(a.recv())
    check("chat.create not found 404", r["type"] == "chat.create_res"
          and r["data"]["code"] == 404)
    # 自己对话框：chat.create 用自身 account → 返回自己信息
    a.send(env("chat.create", {"account": A}))
    r = json.loads(a.recv())
    check("chat.create self by own account", r["type"] == "chat.create_res"
          and r["data"]["code"] == 0 and r["data"]["data"]["userId"] == alice
          and r["data"]["data"]["account"] == A
          and r["data"]["data"]["username"] == "小爱")
    d = WS()
    d.send(env("chat.create", {"account": B}))
    r = json.loads(d.recv())
    check("chat.create not login 401", r["type"] == "error" and r["data"]["code"] == 401)
    d.close()

    # 自己对话框：目标为自己的 userId（发送给自己的消息只回 ack、不投递 msg.recv，
    # 消息由客户端本地显示）。用独立账号测试，避免 alice 已被登录限流用例锁窗。
    # v1.8：to 传自身登录账号 account（字符串/数字）即自己对话框。
    c1 = WS()
    r = register(c1, C, "pw1234")
    check("self register ok", r["type"] == "auth.register_res"
          and r["data"]["code"] == 0)
    carol = r["data"]["data"]["userId"]
    r = login(c1, C, "pw1234")
    check("self login ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)
    # 自己对话框（to==自己账号）：只回 ack、不投递 msg.recv（协议 §4）
    c1.send(env("msg.send", {"to": C, "content": "x", "clientMsgId": "self0"}))
    r = json.loads(c1.recv())
    check("self to acct ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    try:
        r = c1.recv(0.5)
    except socket.timeout:
        r = None
    check("self to acct no msg.recv echo", r is None)
    # to 传自身登录账号（字符串/数字）也是自己对话框
    for i, target in enumerate((C, int(C))):
        c1.send(env("msg.send", {"to": target, "content": "to self acct",
                                 "clientMsgId": "selfacct%d" % i}))
        r = json.loads(c1.recv())
        check("self account ack ok (%s)" % type(target).__name__,
              r["type"] == "msg.ack" and r["data"]["code"] == 0)
        try:
            r = c1.recv(0.5)
        except socket.timeout:
            r = None
        check("self account no msg.recv echo (%s)" % type(target).__name__, r is None)
    # 发给自己的 userId：只回 ack，不投递 msg.recv
    c1.send(env("msg.send", {"to": carol, "content": "to self", "clientMsgId": "self1"}))
    r = json.loads(c1.recv())
    check("self ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    try:
        r = c1.recv(0.5)
    except socket.timeout:
        r = None
    check("self no msg.recv echo", r is None)

    # 自己对话框多端：两会话在线，发给自己的消息两会话都不再收到 msg.recv
    c2 = WS()
    r = login(c2, C, "pw1234")
    check("self second session login ok", r["type"] == "auth.login_res"
          and r["data"]["code"] == 0)
    c1.send(env("msg.send", {"to": carol, "content": "multi self", "clientMsgId": "self2"}))
    r = json.loads(c1.recv())
    check("self multi ack ok", r["data"]["code"] == 0)
    try:
        r1 = c1.recv(0.5)
    except socket.timeout:
        r1 = None
    try:
        r2 = c2.recv(0.5)
    except socket.timeout:
        r2 = None
    check("self multi NOT delivered to any session", r1 is None and r2 is None)
    # v1.8：自己对话框落库，chat.history 可查到（self0/selfacct0/selfacct1/self1/self2 共 5 条）
    c1.send(env("chat.history", {"to": C}))
    r = json.loads(c1.recv())
    check("self history persisted", r["type"] == "chat.history_res"
          and r["data"]["code"] == 0
          and len(r["data"]["data"]["msgs"]) >= 3)
    # 自己对话框新建：chat.create 用自身 account → 返回自己信息
    c1.send(env("chat.create", {"account": C}))
    r = json.loads(c1.recv())
    check("self chat.create by own account", r["type"] == "chat.create_res"
          and r["data"]["code"] == 0 and r["data"]["data"]["userId"] == carol
          and r["data"]["data"]["account"] == C)
    # 发给别人的消息不回显给自己：发给 bob 后，自己（c1/c2）不应收到
    c1.send(env("msg.send", {"to": bob, "content": "no echo", "clientMsgId": "self3"}))
    r = json.loads(c1.recv())
    check("no echo ack ok", r["data"]["code"] == 0)
    json.loads(b.recv())  # bob 收到
    try:
        echo = c2.recv(0.5)
    except socket.timeout:
        echo = None
    check("no echo to self sessions", echo is None)
    c2.close()
    c1.close()

    # chatType 校验：缺省 single 正常；非 single 返回 400
    a.send(env("msg.send", {"to": bob, "content": "x", "clientMsgId": "ct1",
                            "chatType": "single"}))
    r = json.loads(a.recv())
    check("chatType single ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    json.loads(b.recv())  # 消费单聊推送
    a.send(env("msg.send", {"to": bob, "content": "x", "clientMsgId": "ct2",
                            "chatType": "invalid"}))
    r = json.loads(a.recv())
    check("chatType invalid 400", r["type"] == "msg.ack" and r["data"]["code"] == 400)

    # clientMsgId 幂等去重：同 id 重发只投递一次
    a.send(env("msg.send", {"to": bob, "content": "dup", "clientMsgId": "dup1"}))
    r = json.loads(a.recv())
    check("dup first ack ok", r["data"]["code"] == 0)
    r = json.loads(b.recv())
    check("dup first delivered", r["data"]["clientMsgId"] == "dup1")
    a.send(env("msg.send", {"to": bob, "content": "dup", "clientMsgId": "dup1"}))
    r = json.loads(a.recv())
    check("dup second ack ok", r["data"]["code"] == 0)
    b.s.settimeout(0.5)
    try:
        dup = b.recv()
    except socket.timeout:
        dup = None
    b.s.settimeout(5)
    check("dup second NOT delivered", dup is None)

    # 目标不存在
    r = send_msg(a, 999999, "x", "m2")
    check("user not found 404", r["type"] == "msg.ack" and r["data"]["code"] == 404)

    # 空内容
    r = send_msg(a, bob, "", "m3")
    check("empty content 400", r["type"] == "msg.ack" and r["data"]["code"] == 400)

    # 未知类型
    a.send(env("badpacket", {}))
    r = json.loads(a.recv())
    check("unknown type 400", r["type"] == "error" and r["data"]["code"] == 400)

    # 未登录
    d = WS()
    r = send_msg(d, bob, "x", "m4")
    check("not logged in 401", r["type"] == "error" and r["data"]["code"] == 401)
    d.close()

    # 消息发送限流：携带 clientMsgId 每用户 10 秒最多 30 条，第 31 条回 429
    sink = WS()
    r = register(sink, SINK, "pw1234")
    sink_id = r["data"]["data"]["userId"]
    r = login(sink, SINK, "pw1234")
    assert r["data"]["code"] == 0, r
    spammer = WS()
    r = register(spammer, SPAM, "pw1234")
    r = login(spammer, SPAM, "pw1234")
    assert r["data"]["code"] == 0, r
    got_429 = False
    for i in range(31):
        spammer.send(env("msg.send", {"to": sink_id, "content": "f%d" % i,
                                      "clientMsgId": "spam%d" % i}))
        r = json.loads(spammer.recv())
        if r["data"]["code"] == 429:
            got_429 = True
            break
    check("msg send rate limit 429", got_429)
    spammer.close()
    sink.close()

    # 空 clientMsgId 更严限流：10 秒窗口最多 5 条，第 6 条回 429
    sink2 = WS()
    r = register(sink2, SINK2, "pw1234")
    sink2_id = r["data"]["data"]["userId"]
    r = login(sink2, SINK2, "pw1234")
    assert r["data"]["code"] == 0, r
    spammer2 = WS()
    r = register(spammer2, SPAM2, "pw1234")
    r = login(spammer2, SPAM2, "pw1234")
    assert r["data"]["code"] == 0, r
    got_429 = False
    for i in range(6):
        spammer2.send(env("msg.send", {"to": sink2_id, "content": "n%d" % i}))
        r = json.loads(spammer2.recv())
        if r["data"]["code"] == 429:
            got_429 = True
            break
    check("msg send empty clientMsgId stricter 429", got_429)
    spammer2.close()
    sink2.close()

    # 登出
    b.send(env("auth.logout", {}))
    r = json.loads(b.recv())
    check("logout ok", r["type"] == "auth.logout_res" and r["data"]["code"] == 0)
    b.close()

    # 离线补发
    r = send_msg(a, bob, "offline msg", "m5")
    check("offline ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    b2 = WS()
    r = login(b2, B, "pw1234")
    check("relogin bob ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)
    r = json.loads(b2.recv())
    check("offline delivery", r["type"] == "msg.offline"
          and len(r["data"]["msgs"]) == 1
          and r["data"]["msgs"][0]["content"] == "offline msg")
    # v1.7：离线消息 from 为发送方登录账号 account（userId 对客户端不透明）
    check("offline from is account", r["data"]["msgs"][0]["from"] == A)
    # 离线确认：msg.offline_ack 后不再补发
    b2.send(env("msg.offline_ack", {"messageIds": [r["data"]["msgs"][0]["messageId"]]}))
    r = json.loads(b2.recv())
    check("offline_ack ok", r["type"] == "msg.offline_ack_res" and r["data"]["code"] == 0)
    b2.close()

    # v1.8：chat.history 的 to 为登录账号 account；自己对话框（to==自己账号）已落库，
    # 历史可查（见 self history persisted 用例）；与无历史的其他账号返回空列表
    a.send(env("chat.history", {"to": B}))
    r = json.loads(a.recv())
    check("history to account ok", r["type"] == "chat.history_res"
          and r["data"]["code"] == 0 and len(r["data"]["data"]["msgs"]) >= 1)
    hist_msgs = [m for m in r["data"]["data"]["msgs"]]
    check("history from is account", hist_msgs and all(m["from"] == A for m in hist_msgs))
    a.send(env("chat.history", {"to": C}))
    r = json.loads(a.recv())
    check("history to carol empty ok", r["type"] == "chat.history_res"
          and r["data"]["code"] == 0 and r["data"]["data"]["msgs"] == [])
    a.send(env("chat.history", {"to": A}))
    r = json.loads(a.recv())
    check("history to own account ok", r["type"] == "chat.history_res"
          and r["data"]["code"] == 0)
    a.send(env("chat.history", {"to": NOBODY}))
    r = json.loads(a.recv())
    check("history to missing 404", r["type"] == "chat.history_res"
          and r["data"]["code"] == 404)

    # v1.8：msg.read 的 from 为登录账号 account；自己对话框（from==自己账号）直接成功
    a.send(env("msg.read", {"from": B}))
    r = json.loads(a.recv())
    check("read from account ok", r["type"] == "msg.read_res" and r["data"]["code"] == 0)
    a.send(env("msg.read", {"from": A}))
    r = json.loads(a.recv())
    check("read from own account ok", r["type"] == "msg.read_res"
          and r["data"]["code"] == 0)
    a.send(env("msg.read", {"from": C}))
    r = json.loads(a.recv())
    check("read from carol ok", r["type"] == "msg.read_res" and r["data"]["code"] == 0)

    a.close()

    # ---- v1.8 联系人体系 ----
    # 准备独立账号：用 D/E/F 避免与前面账号干扰
    D = base + "8"; E = base + "9"; F = base + "10"
    d = WS(); register(d, D, "pw1234"); login(d, D, "pw1234")
    e = WS(); register(e, E, "pw1234"); login(e, E, "pw1234")
    f = WS(); register(f, F, "pw1234"); login(f, F, "pw1234")

    # 搜索：按账号关键字搜到对方
    d.send(env("user.search", {"keyword": E}))
    r = json.loads(d.recv())
    check("search by account", r["type"] == "user.search_res" and r["data"]["code"] == 0
          and any(u["account"] == E for u in r["data"]["data"]["users"]))
    # 搜索到自己不出现在结果
    d.send(env("user.search", {"keyword": D}))
    r = json.loads(d.recv())
    check("search excludes self", r["type"] == "user.search_res"
          and all(u["account"] != D for u in r["data"]["data"]["users"]))

    # 添加请求 → 对方收到 contact.request
    d.send(env("contact.request", {"account": E}))
    r = json.loads(d.recv())
    check("contact request ok", r["type"] == "contact.request_res" and r["data"]["code"] == 0)
    r = json.loads(e.recv())
    check("contact request pushed", r["type"] == "contact.request"
          and r["data"]["from"] == D)
    # 重复请求 → 409
    d.send(env("contact.request", {"account": E}))
    r = json.loads(d.recv())
    check("contact request dup 409", r["type"] == "contact.request_res"
          and r["data"]["code"] == 409)
    # 对方查看 pending 请求
    e.send(env("contact.requests", {}))
    r = json.loads(e.recv())
    check("contact requests list", r["type"] == "contact.requests_res"
          and r["data"]["code"] == 0
          and any(x["account"] == D for x in r["data"]["data"]["requests"]))
    # 同意 → 双方联系人；请求方收到 contact.added
    e.send(env("contact.accept", {"account": D}))
    r = json.loads(e.recv())
    check("contact accept ok", r["type"] == "contact.accept_res" and r["data"]["code"] == 0)
    r = json.loads(d.recv())
    check("contact added pushed", r["type"] == "contact.added" and r["data"]["account"] == E)
    # 联系人列表
    d.send(env("contact.list", {}))
    r = json.loads(d.recv())
    check("contact list has E", r["type"] == "contact.list_res"
          and any(x["account"] == E for x in r["data"]["data"]["contacts"]))
    # 成为联系人后互发消息成功
    r = send_msg(d, E, "hi contact", "ct1")
    check("contact send ok", r["data"]["code"] == 0)
    r = json.loads(e.recv())
    check("contact recv ok", r["type"] == "msg.recv" and r["data"]["from"] == D)

    # 已是联系人：再次 request → 409
    d.send(env("contact.request", {"account": E}))
    r = json.loads(d.recv())
    check("contact request already contact 409", r["type"] == "contact.request_res"
          and r["data"]["code"] == 409)
    # 不存在目标：request → 404
    d.send(env("contact.request", {"account": NOBODY}))
    r = json.loads(d.recv())
    check("contact request missing 404", r["type"] == "contact.request_res"
          and r["data"]["code"] == 404)

    # 拒绝流程：F 请求 D，D 拒绝
    f.send(env("contact.request", {"account": D}))
    json.loads(f.recv())
    json.loads(d.recv())  # contact.request 推送
    d.send(env("contact.reject", {"account": F}))
    r = json.loads(d.recv())
    check("contact reject ok", r["type"] == "contact.reject_res" and r["data"]["code"] == 0)
    f.send(env("contact.list", {}))
    r = json.loads(f.recv())
    check("rejected not in contacts", r["data"]["code"] == 0
          and all(x["account"] != D for x in r["data"]["data"]["contacts"]))
    # 拒绝后请求关闭：F 重新请求 D 应成功（不再被旧 rejected 行拦截）
    f.send(env("contact.request", {"account": D}))
    r = json.loads(f.recv())
    check("contact request after reject ok", r["type"] == "contact.request_res"
          and r["data"]["code"] == 0)
    json.loads(d.recv())  # 消费 contact.request 推送

    # 删除联系人：D 删除 E，双向解除（E 在线，先收到 contact.deleted 推送）
    d.send(env("contact.delete", {"account": E}))
    r = json.loads(d.recv())
    check("contact delete ok", r["type"] == "contact.delete_res" and r["data"]["code"] == 0)
    r = json.loads(e.recv())
    check("contact deleted pushed", r["type"] == "contact.deleted"
          and r["data"]["account"] == D)
    e.send(env("contact.list", {}))
    r = json.loads(e.recv())
    check("contact deleted both sides", r["data"]["code"] == 0
          and all(x["account"] != D for x in r["data"]["data"]["contacts"]))

    # 删除后重新添加：E 重新请求 D 应成功（旧 accepted 行不再拦截）
    e.send(env("contact.request", {"account": D}))
    r = json.loads(e.recv())
    check("contact re-request after delete ok", r["type"] == "contact.request_res"
          and r["data"]["code"] == 0)
    json.loads(d.recv())  # 消费 contact.request 推送

    # 互相请求：D 也请求 E（此时 E→D 的 pending 已存在，D→E 从 rejected 重置）
    d.send(env("contact.request", {"account": E}))
    r = json.loads(d.recv())
    check("mutual request ok", r["type"] == "contact.request_res"
          and r["data"]["code"] == 0)
    json.loads(e.recv())  # e 收到 D 的请求推送
    # E 接受 D 的请求 → 双方成为联系人；反向（E→D）待处理请求应一并关闭
    e.send(env("contact.accept", {"account": D}))
    r = json.loads(e.recv())
    check("mutual accept ok", r["type"] == "contact.accept_res"
          and r["data"]["code"] == 0)
    json.loads(d.recv())  # d 收到 contact.added
    d.send(env("contact.requests", {}))
    r = json.loads(d.recv())
    check("mutual accept cleans reverse pending", r["data"]["code"] == 0
          and all(x["account"] != E for x in r["data"]["data"]["requests"]))
    e.send(env("contact.requests", {}))
    r = json.loads(e.recv())
    check("mutual accept cleans forward pending", r["data"]["code"] == 0
          and all(x["account"] != D for x in r["data"]["data"]["requests"]))

    # ---- v1.8.1 自己联系人 ----
    # 注册用户 contact.list 恒含"自己"且排第一位
    selfc = WS(); register(selfc, base + "50", "pw1234", nick="小己")
    login(selfc, base + "50", "pw1234")
    selfc.send(env("contact.list", {}))
    r = json.loads(selfc.recv())
    contacts = r["data"]["data"]["contacts"]
    check("contact list contains self", r["data"]["code"] == 0
          and any(x["account"] == base + "50" for x in contacts))
    check("contact self first", contacts and contacts[0]["account"] == base + "50"
          and contacts[0]["username"] == "小己")
    # 登录不重复插入：再登一次（换连接），contact.list 仍只有一条自己
    selfc2 = WS(); login(selfc2, base + "50", "pw1234")
    selfc2.send(env("contact.list", {}))
    r = json.loads(selfc2.recv())
    cnt = sum(1 for x in r["data"]["data"]["contacts"] if x["account"] == base + "50")
    check("self contact not duplicated on login", r["data"]["code"] == 0 and cnt == 1)
    selfc.close(); selfc2.close()

    d.close(); e.close(); f.close()

    # ---- v1.8 隐私开关 ----
    g = WS(); register(g, G, "pw1234"); login(g, G, "pw1234")
    h = WS(); register(h, H, "pw1234"); login(h, H, "pw1234")
    # 默认三开关全开
    g.send(env("settings.get", {}))
    r = json.loads(g.recv())
    check("settings default on", r["type"] == "settings.get_res"
          and r["data"]["data"]["allowTempChat"] and r["data"]["data"]["searchable"]
          and r["data"]["data"]["allowAdd"])
    # 关临时对话：非联系人 h → g 发送 403
    g.send(env("settings.set", {"allowTempChat": False}))
    r = json.loads(g.recv())
    check("settings set temp off", r["type"] == "settings.set_res" and r["data"]["code"] == 0)
    r = send_msg(h, G, "temp off", "t1")
    check("send to temp-off 403", r["type"] == "msg.ack" and r["data"]["code"] == 403)
    # 关搜索：搜不到
    g.send(env("settings.set", {"searchable": False}))
    json.loads(g.recv())
    h.send(env("user.search", {"keyword": G}))
    r = json.loads(h.recv())
    check("searchable off not found", r["data"]["code"] == 0
          and all(u["account"] != G for u in r["data"]["data"]["users"]))
    # 关添加：request 403
    g.send(env("settings.set", {"allowAdd": False}))
    json.loads(g.recv())
    h.send(env("contact.request", {"account": G}))
    r = json.loads(h.recv())
    check("allow_add off 403", r["type"] == "contact.request_res" and r["data"]["code"] == 403)
    # 恢复（不影响清理）
    g.close(); h.close()

    # ---- v1.9 群聊 ----
    # a/b 在前文已 close，此处重建会话复用 A/B 账号（A 仍处登录限流 60s 窗内故用 token 重连）
    a = WS(); a.send(env("auth.token", {"token": alice_token})); json.loads(a.recv())
    b = WS(); login(b, B, "pw1234")
    G1 = base + "60"; G2 = base + "61"
    g1 = WS(); register(g1, G1, "pw1234", nick="群友1"); login(g1, G1, "pw1234")
    g2 = WS(); register(g2, G2, "pw1234", nick="群友2"); login(g2, G2, "pw1234")
    # A/B 建联（群邀请要求联系人）
    a.send(env("contact.request", {"account": B}))
    r = json.loads(a.recv())
    if r["data"]["code"] == 0:
        json.loads(b.recv())
        b.send(env("contact.accept", {"account": A}))
        json.loads(b.recv()); json.loads(a.recv())
    # 建群
    a.send(env("group.create", {"name": "测试群"}))
    r = json.loads(a.recv())
    check("group create ok", r["type"] == "group.create_res" and r["data"]["code"] == 0)
    gid = r["data"]["data"]["groupId"]
    # 建群空名 400
    a.send(env("group.create", {"name": ""}))
    r = json.loads(a.recv())
    check("group create empty 400", r["type"] == "group.create_res"
          and r["data"]["code"] == 400)
    # 拉人：bob（联系人）成功；g1（非联系人）409
    a.send(env("group.invite", {"groupId": gid, "account": B}))
    r = json.loads(a.recv())
    check("group invite contact ok", r["type"] == "group.invite_res"
          and r["data"]["code"] == 0)
    r = json.loads(b.recv())
    check("group added pushed", r["type"] == "group.added"
          and r["data"]["groupId"] == gid)
    json.loads(a.recv())  # 消费 group.member_changed（拉人成功推送群内成员，含邀请人）
    a.send(env("group.invite", {"groupId": gid, "account": G1}))
    r = json.loads(a.recv())
    check("group invite non-contact 409", r["type"] == "group.invite_res"
          and r["data"]["code"] == 409)
    # 非群主拉人 403（bob 不是群主）
    b.send(env("group.invite", {"groupId": gid, "account": G1}))
    r = json.loads(b.recv())
    check("group invite non-owner 403", r["type"] == "group.invite_res"
          and r["data"]["code"] == 403)
    # 群发消息：a 发，b 收到（from/groupId/chatType 校验），a 不回显
    a.send(env("msg.send", {"to": gid, "content": "hello group", "clientMsgId": "g1",
                            "chatType": "group"}))
    r = json.loads(a.recv())
    check("group send ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    r = json.loads(b.recv())
    check("group recv ok", r["type"] == "msg.recv" and r["data"]["from"] == A
          and r["data"]["groupId"] == gid and r["data"]["chatType"] == "group")
    a.s.settimeout(0.5)
    try:
        echo = a.recv()
    except socket.timeout:
        echo = None
    a.s.settimeout(5)
    check("group no echo to sender", echo is None)
    # 非成员 g1 发送 403
    g1.send(env("msg.send", {"to": gid, "content": "x", "clientMsgId": "g2",
                             "chatType": "group"}))
    r = json.loads(g1.recv())
    check("group send non-member 403", r["type"] == "msg.ack" and r["data"]["code"] == 403)
    # 不存在的群 404
    a.send(env("msg.send", {"to": 999999999, "content": "x", "clientMsgId": "g3",
                            "chatType": "group"}))
    r = json.loads(a.recv())
    check("group send missing 404", r["type"] == "msg.ack" and r["data"]["code"] == 404)
    # 群历史：b 可查，g1（从未加入）403
    b.send(env("group.history", {"groupId": gid}))
    r = json.loads(b.recv())
    check("group history ok", r["type"] == "group.history_res"
          and r["data"]["code"] == 0 and len(r["data"]["data"]["msgs"]) >= 1)
    g1.send(env("group.history", {"groupId": gid}))
    r = json.loads(g1.recv())
    check("group history non-member 403", r["type"] == "group.history_res"
          and r["data"]["code"] == 403)
    # 群列表与成员
    b.send(env("group.list", {}))
    r = json.loads(b.recv())
    check("group list ok", r["type"] == "group.list_res"
          and any(x["groupId"] == gid for x in r["data"]["data"]["groups"]))
    b.send(env("group.members", {"groupId": gid}))
    r = json.loads(b.recv())
    check("group members ok", r["type"] == "group.members_res"
          and len(r["data"]["data"]["members"]) == 2)
    # 退群：b 退群后发消息 403；b 可查在群期间历史；群主 a 退群 403
    b.send(env("group.leave", {"groupId": gid}))
    r = json.loads(b.recv())
    check("group leave ok", r["type"] == "group.leave_res" and r["data"]["code"] == 0)
    json.loads(a.recv())  # 消费 group.member_changed（退群推送群内剩余成员）
    b.send(env("msg.send", {"to": gid, "content": "x", "clientMsgId": "g4",
                            "chatType": "group"}))
    r = json.loads(b.recv())
    check("group send after leave 403", r["type"] == "msg.ack" and r["data"]["code"] == 403)
    b.send(env("group.history", {"groupId": gid}))
    r = json.loads(b.recv())
    check("group history ex-member ok", r["type"] == "group.history_res"
          and r["data"]["code"] == 0 and len(r["data"]["data"]["msgs"]) >= 1)
    a.send(env("group.leave", {"groupId": gid}))
    r = json.loads(a.recv())
    check("group owner leave 403", r["type"] == "group.leave_res"
          and r["data"]["code"] == 403)
    g1.close(); g2.close()
    a.close(); b.close()

    # 清理本次注册的测试账号及其关联消息（精确账号列表，不误删真实用户）
    import subprocess
    used = [A, B, C, SINK, SPAM, SINK2, SPAM2, D, E, F, G, H, base + "50", G1, G2]
    inlist = ",".join("'%s'" % x for x in used)
    # 群表清理必须在 users 删除之前（按 account 查 user_id，无级联外键）
    subprocess.run(["bash", "-c",
        "MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -e \""
        "USE IMS_reactor_db; "
        "DELETE FROM group_members WHERE user_id IN (SELECT id FROM users WHERE account IN (%s)); "
        "DELETE FROM \`groups\` WHERE owner_id IN (SELECT id FROM users WHERE account IN (%s));\" 2>/dev/null" % (inlist, inlist)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["bash", "-c",
        "MYSQL_PWD=520010 mysql -u IMS_jack -h 127.0.0.1 -e \""
        "USE IMS_reactor_db; "
        "DELETE m FROM messages m JOIN users u ON m.from_user=u.id OR m.to_user=u.id "
        "WHERE u.account IN (%s); "
        "DELETE FROM users WHERE account IN (%s);\" 2>/dev/null" % (inlist, inlist)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    fails = [n for n, c in results if not c]
    print(f"\n{len(results)} tests, {len(fails)} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
