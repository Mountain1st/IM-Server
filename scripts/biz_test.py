#!/usr/bin/env python3
"""IM 服务器业务回归测试。

用法: python3 scripts/biz_test.py [host] [port]
无第三方依赖（直接用 socket 实现 WebSocket 客户端）。
覆盖: 注册/重复注册/登录/错误密码/登录限流/消息收发/404/400/401/登出/离线补发/消息发送限流。
"""
import base64
import hashlib
import hmac
import json
import os
import socket
import struct
import sys
import time
import urllib.parse
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9191

results = []
skipped = []


def check(name, cond):
    results.append((name, cond))
    print(("PASS " if cond else "FAIL ") + name)


def skip(name, reason=""):
    # 记录并显式打印跳过项（环境不满足的用例段），最终统计可体现
    skipped.append((name, reason))
    print("SKIP " + name + ((" (%s)" % reason) if reason else ""))


def media_ok(resp):
    # media.*_res 顶层 code==0 判定（防御式：非 dict/字段缺失返回 False，不抛 KeyError）
    if not isinstance(resp, dict):
        return False
    data = resp.get("data")
    return isinstance(data, dict) and data.get("code") == 0


def media_data(resp):
    # 取 media.*_res 的 data.data（code!=0 或字段缺失返回 {}，避免 KeyError）
    if not media_ok(resp):
        return {}
    d = resp.get("data", {}).get("data")
    return d if isinstance(d, dict) else {}


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


# 测试用 MySQL 密码：从环境变量取（test_biz.sh 经 db_env.sh 提供；缺省与 compose 模板一致）
MYSQL_PWD_ENV = os.environ.get("IM_MYSQL_PASSWORD", "change-me")
MYSQL_PORT_ENV = os.environ.get("IM_MYSQL_PORT", "3306")


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


def send_msg(ws, to, content, cid, msg_type=None, media_id=None, chat_type=None):
    # msg_type 为 None 时不带该字段（保持原文本用例行为不变）
    # media_id 为图片消息关联的 fileId（msgType=image 时必带）
    # chat_type 为 None 时不带该字段（撤回段群发用例需要 group）
    d = {"to": to, "content": content, "clientMsgId": cid}
    if msg_type is not None:
        d["msgType"] = msg_type
    if media_id is not None:
        d["mediaId"] = media_id
    if chat_type is not None:
        d["chatType"] = chat_type
    ws.send(env("msg.send", d))
    return json.loads(ws.recv())


def send(ws, typ, data):
    # 通用指令发送：返回解析后的响应信封（非 JSON 则返回 None）
    ws.send(env(typ, data))
    raw = ws.recv()
    return json.loads(raw) if raw else None


# 对象存储直传/下载辅助：测试机 MinIO 静态二进制监听 127.0.0.1:9000，
# 预签名 URL 沿用服务端 IM_S3_ENDPOINT，故直接对 URL 发 PUT/GET 做真实字节校验。
def put(url, body):
    # urllib PUT 上传对象存储（预签名 PUT URL，方法必须 PUT）
    req = urllib.request.Request(url, data=body, method="PUT")
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.status


def get(url):
    # urllib GET 下载对象存储（预签名 GET URL），返回响应字节
    with urllib.request.urlopen(url, timeout=10) as resp:
        return resp.read()


# 任务 4 用：1x1 透明 PNG 的 base64（解码后 1x1，图片用例直传对象存储）
PNG_1X1_B64 = ("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGA"
               "hKmMIQAAAABJRU5ErkJggg==")


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
    check("token session send ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    json.loads(b.recv())
    # v2.2：d 是 A 的第二端，其发送的消息回显到 A 的其他端（a）；消费之
    json.loads(a.recv())
    # 无效 token
    d.send(env("auth.token", {"token": "bogus"}))
    r = json.loads(d.recv())
    check("token reconnect invalid 401", r["type"] == "auth.token_res"
          and r["data"]["code"] == 401)
    d.close()

    # 消息收发
    r = send_msg(a, bob, "hello bob", "m1")
    print("DEBUG m1 r:", json.dumps(r)[:250])
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
    # to 传自身登录账号（字符串/数字）也是自己对话框。
    # 注意：数字形态仅在 int 往返无损时才有意义——账号以 0 开头时 int(C) 会丢前导零
    # 变成另一个数（非账号非合法 userId），属客户端无法无损传数字的账号，跳过该子例。
    targets = [("str", C)]
    if str(int(C)) == C:
        targets.append(("int", int(C)))
    for i, (tname, target) in enumerate(targets):
        c1.send(env("msg.send", {"to": target, "content": "to self acct",
                                 "clientMsgId": "selfacct%d" % i}))
        r = json.loads(c1.recv())
        check("self account ack ok (%s)" % tname,
              r["type"] == "msg.ack" and r["data"]["code"] == 0)
        try:
            r = c1.recv(0.5)
        except socket.timeout:
            r = None
        check("self account no msg.recv echo (%s)" % tname, r is None)
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
    check("self multi ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
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
    # 发给别人的消息回显到自己的其他端（v2.2 多端一致）：c1 发给 bob，c2 应收到
    c1.send(env("msg.send", {"to": bob, "content": "no echo", "clientMsgId": "self3"}))
    r = json.loads(c1.recv())
    check("no echo ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    json.loads(b.recv())  # bob 收到
    echo = json.loads(c2.recv())
    check("self echo to other sessions (v2.2)", echo is not None
          and echo["type"] == "msg.recv" and echo["data"]["from"] == C
          and echo["data"]["content"] == "no echo")
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

    # ---- 任务 10：单聊图片消息改走对象存储直传 ----
    # 流：presign → 真机 PUT 到 MinIO → commit → msg.send(mediaId=fileId)
    #     → 接收方 msg.recv 带 mediaId → 查看方 media.url → GET 校验字节一致。
    # 账号后缀 75/76：避开群满员用例的 cap 段与 NOBODY(77)/OVER(78)/M段(80~83)
    # 环境门禁：服务端未配置对象存储（IM_S3_* 缺失 → store 未启用 → presign
    # 返回 503）时，显式跳过整段媒体用例（不静默、不 KeyError 中断后续用例）。
    medias_enabled = bool(os.environ.get("IM_S3_ENDPOINT"))
    PIMG = base + "75"
    QIMG = base + "76"
    # 越权用例账号在媒体段外定义：无论媒体段是否跳过，清理列表都需引用它
    CIMG = base + "86"
    if medias_enabled:
      pim = WS()
      r = register(pim, PIMG, "pw1234")
      check("image register ok", r["type"] == "auth.register_res" and r["data"]["code"] == 0)
      r = login(pim, PIMG, "pw1234")
      check("image login ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)
      qim = WS()
      r = register(qim, QIMG, "pw1234")
      check("image peer register ok", r["type"] == "auth.register_res" and r["data"]["code"] == 0)
      r = login(qim, QIMG, "pw1234")
      check("image peer login ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)

      png = base64.b64decode(PNG_1X1_B64)

      # 负例：未登录 presign 拒绝（onEnvelope 登录门禁，error 401）
      anon = WS()
      r = send(anon, "media.presign", {"fileName": "p.png", "mimeType": "image/png",
                                       "size": len(png), "kind": "image"})
      check("media presign not login 401",
            r["type"] == "error" and r["data"]["code"] == 401)
      anon.close()

      # 正例：presign → 真机 PUT → commit
      r = send(pim, "media.presign", {"fileName": "p.png", "mimeType": "image/png",
                                      "size": len(png), "kind": "image"})
      check("media presign ok", media_ok(r))
      upload_url = media_data(r).get("uploadUrl", "")
      fid = media_data(r).get("fileId", "")
      check("media presign fileId 32hex",
            len(fid) == 32 and all(c in "0123456789abcdef" for c in fid))
      check("media PUT upload ok", bool(upload_url) and put(upload_url, png) in (200, 204))
      # 负例：commit 未上传对象失败（presign 后不 PUT，HEAD 不存在 → 400）
      r = send(pim, "media.presign", {"fileName": "missing.png", "mimeType": "image/png",
                                      "size": len(png), "kind": "image"})
      fid_missing = media_data(r).get("fileId", "")
      r = send(pim, "media.commit", {"fileId": fid_missing, "size": len(png),
                                     "width": 1, "height": 1})
      check("media commit missing object rejected",
            r["type"] == "media.commit_res" and r["data"]["code"] == 400)
      # 负例：伪造 fileId commit 失败（查无记录 → 404）
      r = send(pim, "media.commit", {"fileId": "f" * 32, "size": len(png),
                                     "width": 1, "height": 1})
      check("media commit forged fileId 404",
            r["type"] == "media.commit_res" and r["data"]["code"] == 404)
      # 负例：他人 fileId commit 拒绝（非 uploader → 403）
      r = send(qim, "media.commit", {"fileId": fid, "size": len(png), "width": 1, "height": 1})
      check("media commit other uploader 403",
            r["type"] == "media.commit_res" and r["data"]["code"] == 403)
      # 正例：commit 成功 + 重复 commit 幂等
      r = send(pim, "media.commit", {"fileId": fid, "size": len(png), "width": 1, "height": 1})
      check("media commit ok", r["type"] == "media.commit_res" and r["data"]["code"] == 0)
      r = send(pim, "media.commit", {"fileId": fid, "size": len(png), "width": 1, "height": 1})
      check("media commit idempotent",
            r["type"] == "media.commit_res" and r["data"]["code"] == 0)

      # 正例：发图片消息 → 接收方收 msg.recv 带 mediaId(fileId)
      r = send_msg(pim, QIMG, "", "img-ok-1", msg_type="image", media_id=fid)
      check("image send ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
      check("image ack has no msg_type", r["type"] == "msg.ack"
            and "msgType" not in r["data"] and "msg_type" not in r["data"])
      r = json.loads(qim.recv())
      check("image recv mediaId", r["type"] == "msg.recv"
            and r["data"].get("mediaId") == fid and r["data"].get("msgType") == "image")
      # 查看方 media.url → GET URL → 下载字节一致
      r = send(qim, "media.url", {"fileId": fid})
      check("media url receiver ok", media_ok(r))
      ent = media_data(r).get("urls", {}).get(fid, {})
      check("media url receiver returns url",
            "url" in ent and ent["url"].startswith("http"))
      check("media GET bytes match",
            "url" in ent and get(ent["url"]) == png)
      # 发送方（uploader/from）同样可达
      r = send(pim, "media.url", {"fileId": fid})
      sent_ent = media_data(r).get("urls", {}).get(fid, {})
      check("media url sender ok", sent_ent.get("url", "").startswith("http"))
      # 负例：media.url 越权（非消息相关方 → 403）
      cim = WS(); register(cim, CIMG, "pw1234"); login(cim, CIMG, "pw1234")
      r = send(cim, "media.url", {"fileId": fid})
      check("media url stranger 403", media_data(r).get("urls", {}).get(fid, {}).get("code") == 403)
      cim.close()
      # 负例：伪造 fileId 的 msg.send 拒绝（404）
      r = send_msg(pim, QIMG, "", "img-forged-1", msg_type="image", media_id="f" * 32)
      check("image send forged mediaId 404", r["type"] == "msg.ack" and r["data"]["code"] == 404)
      # 负例：他人 fileId 的 msg.send 拒绝（非 uploader → 403）
      r = send_msg(qim, PIMG, "", "img-other-1", msg_type="image", media_id=fid)
      check("image send other uploader 403", r["type"] == "msg.ack" and r["data"]["code"] == 403)
      # 负例：非法 msgType 白名单外 → 400
      pim.send(env("msg.send", {"to": QIMG, "content": "x", "clientMsgId": "img-bad-0",
                                "msgType": "video"}))
      r = json.loads(pim.recv())
      check("image bad msgType 400", r["type"] == "msg.ack" and r["data"]["code"] == 400)
      # 历史透传：mediaId 行带 msgType=image
      pim.send(env("chat.history", {"to": QIMG}))
      r = json.loads(pim.recv())
      himgs = [m for m in r["data"]["data"]["msgs"] if m.get("msgType") == "image"]
      check("image history has mediaId", len(himgs) >= 1
            and all(m.get("mediaId") == fid for m in himgs))
      # 注：过期 PUT URL 上传失败为可选负例——预签名 TTL 固定 900s，测试不便等待；
      # 单测/集成已在任务 5 覆盖签名过期语义，此处标注未执行。
      pim.close()
      qim.close()
    else:
      skip("media single-chat suite", "IM_S3_ENDPOINT not set (object store disabled)")

    # clientMsgId 幂等去重：同 id 重发只投递一次
    a.send(env("msg.send", {"to": bob, "content": "dup", "clientMsgId": "dup1"}))
    r = json.loads(a.recv())
    check("dup first ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    r = json.loads(b.recv())
    check("dup first delivered", r["data"]["clientMsgId"] == "dup1")
    a.send(env("msg.send", {"to": bob, "content": "dup", "clientMsgId": "dup1"}))
    r = json.loads(a.recv())
    check("dup second ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
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
    # 投递语义 v2：在线消息未 ACK 会在重登补发，故 msgs 可能含历史未确认消息。
    # 断言目标消息存在（而非精确条数）。
    off_msgs = r["data"]["msgs"] if r["type"] == "msg.offline" else []
    target = [m for m in off_msgs if m["content"] == "offline msg"]
    check("offline delivery", len(target) == 1)
    # v1.7：离线消息 from 为发送方登录账号 account（userId 对客户端不透明）
    check("offline from is account", len(target) == 1 and target[0]["from"] == A)
    # 离线确认：msg.offline_ack 后不再补发（本次确认全部已收）
    b2.send(env("msg.offline_ack", {"messageIds": [m["messageId"] for m in off_msgs]}))
    r = json.loads(b2.recv())
    check("offline_ack ok", r["type"] == "msg.offline_ack_res" and r["data"]["code"] == 0)
    # 安全加固：offline_ack 的 messageIds 数组超限（>1000）必须拒绝，不能逐条打 DB
    b3 = WS()
    r = login(b3, B, "pw1234")
    check("relogin bob for ack-limit ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)
    # 投递语义 v2：重登可能先收到未确认消息的 msg.offline 补发，先消费再测超限拒绝
    b3.s.settimeout(1.0)
    try:
        json.loads(b3.recv())
    except Exception:
        pass
    b3.s.settimeout(5)
    b3.send(env("msg.offline_ack", {"messageIds": ["x"] * 1500}))
    r = json.loads(b3.recv())
    check("offline_ack oversized rejected", r["type"] == "msg.offline_ack_res" and r["data"]["code"] == 400)
    b3.close()
    # 安全加固：clientMsgId 超长（>64）必须拒绝（与 DB 列宽对齐，防 Redis 大 key）
    r = send_msg(a, B, "id-len probe", "y" * 65)
    check("clientMsgId oversized rejected", r["type"] == "msg.ack" and r["data"]["code"] == 400)
    b2.close()

    # v2.1：离线消息 TTL 清理——超 7 天未领取的离线消息由后台任务删除
    def sql_q(q):
        import subprocess
        out = subprocess.run(
            ["bash", "-c",
             "MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s -N -e \"%s\" IMS_reactor_db" % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, q)],
            capture_output=True, text=True)
        return out.stdout.strip()

    # message_id 带轮次时间戳后缀：防历史残留（中断轮清理未执行等）撞
    # 唯一索引导致 INSERT 静默失败（2026-09-30 CI 假红根因）
    old_ts = int(time.time()) - 8 * 86400
    for i in range(2):
        sql_q("INSERT INTO messages (message_id,from_user,to_user,content,ts,client_msg_id,"
              "chat_type,group_id,status) VALUES ('purge-old-%s-%d',%d,%d,'stale',%d,'','single',0,0)"
              % (base, i, alice, bob, old_ts + i))
    # 10 分钟前的离线消息：TTL 为 7 天，不应被清理（防 168 秒单位错误回归）
    sql_q("INSERT INTO messages (message_id,from_user,to_user,content,ts,client_msg_id,"
          "chat_type,group_id,status) VALUES ('purge-mid-%s',%d,%d,'recent',%d,'','single',0,0)"
          % (base, alice, bob, int(time.time()) - 600))
    r = send_msg(a, bob, "fresh offline", "purge-new-1")
    check("purge fresh ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    time.sleep(3)  # 测试环境 IM_OFFLINE_PURGE_INTERVAL=2，等一轮清理
    n = sql_q("SELECT COUNT(*) FROM messages WHERE message_id LIKE 'purge-old-%s-%%'"
              " AND from_user=%d AND to_user=%d" % (base, alice, bob))
    check("expired offline purged", n == "0")
    nm = sql_q("SELECT COUNT(*) FROM messages WHERE message_id='purge-mid-%s'"
               " AND from_user=%d AND to_user=%d" % (base, alice, bob))
    check("10min offline kept (TTL=7d)", nm == "1")
    # 保留期二期：已送达/已读的单聊与群消息超 90 天删除；89 天的保留
    old91 = int(time.time()) - 91 * 86400
    old89 = int(time.time()) - 89 * 86400
    sql_q("INSERT INTO messages (message_id,from_user,to_user,content,ts,client_msg_id,"
          "chat_type,group_id,status) VALUES ('purge-h91-%s',%d,%d,'aged',%d,'','single',0,2)"
          % (base, alice, bob, old91))
    sql_q("INSERT INTO messages (message_id,from_user,to_user,content,ts,client_msg_id,"
          "chat_type,group_id,status) VALUES ('purge-h89-%s',%d,%d,'near',%d,'','single',0,2)"
          % (base, alice, bob, old89))
    sql_q("INSERT INTO messages (message_id,from_user,to_user,content,ts,client_msg_id,"
          "chat_type,group_id,status) VALUES ('purge-g91-%s',%d,0,'grp',%d,'','group',%d,1)"
          % (base, alice, old91, 1))
    time.sleep(3)  # 再等一轮清理
    n91 = sql_q("SELECT COUNT(*) FROM messages WHERE message_id IN ('purge-h91-%s','purge-g91-%s')" % (base, base))
    check("aged history purged (90d)", n91 == "0")
    n89 = sql_q("SELECT COUNT(*) FROM messages WHERE message_id='purge-h89-%s'" % base)
    check("89d history kept", n89 == "1")
    nf = sql_q("SELECT COUNT(*) FROM messages WHERE client_msg_id='purge-new-1'"
               " AND from_user=%d AND to_user=%d" % (alice, bob))
    check("fresh offline kept", nf == "1")
    b4 = WS()
    r = login(b4, B, "pw1234")
    r = json.loads(b4.recv())
    contents = [m["content"] for m in r["data"]["msgs"]]
    check("purge login offline has fresh and 10min",
          r["type"] == "msg.offline"
          and {"fresh offline", "recent"}.issubset(set(contents)))
    b4.send(env("msg.offline_ack", {"messageIds": [m["messageId"] for m in r["data"]["msgs"]]}))
    json.loads(b4.recv())
    b4.close()

    # v2.3：缓存低频比对——绕过服务端直改库（SQL 插/删 contacts 行）后，
    # 后台计数比对发现漂移触发缓存重建（测试 IM_OFFLINE_PURGE_INTERVAL=2 一轮 2s），
    # isContact 走新数据：非好友 403 → 直插行 → 放行 → 直删 → 回 403
    X = base + "31"
    Y = base + "32"
    cx = WS()
    r = register(cx, X, "pw1234", nick="缓存甲")
    check("cache sync register x ok", r["type"] == "auth.register_res" and r["data"]["code"] == 0)
    xid = r["data"]["data"]["userId"]
    r = login(cx, X, "pw1234")
    check("cache sync x login ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)
    cy = WS()
    r = register(cy, Y, "pw1234", nick="缓存乙")
    yid = r["data"]["data"]["userId"]
    # 关闭临时对话，保证"非好友"时 msg.send 恒 403（不受 allow_temp_chat 默认值影响）
    # 注意：注册不自动登录，settings.set 需先登录
    r = login(cy, Y, "pw1234")
    check("cache sync y login ok", r["type"] == "auth.login_res" and r["data"]["code"] == 0)
    cy.send(env("settings.set", {"allowTempChat": False}))
    r = json.loads(cy.recv())
    check("cache sync y temp off", r["type"] == "settings.set_res" and r["data"]["code"] == 0)
    r = send_msg(cx, Y, "pre-insert", "cache-pre")
    check("cache sync pre-insert non-friend 403",
          r["type"] == "msg.ack" and r["data"]["code"] == 403)
    sql_q("INSERT INTO contacts (user_id, contact_id) VALUES (%d,%d),(%d,%d)" % (xid, yid, yid, xid))
    time.sleep(3)  # 等一轮缓存比对重建
    r = send_msg(cx, Y, "post-insert", "cache-ins")
    check("cache sync after insert allowed",
          r["type"] == "msg.ack" and r["data"]["code"] == 0)
    sql_q("DELETE FROM contacts WHERE (user_id=%d AND contact_id=%d)"
          " OR (user_id=%d AND contact_id=%d)" % (xid, yid, yid, xid))
    time.sleep(3)  # 再等一轮
    r = send_msg(cx, Y, "post-delete", "cache-del")
    check("cache sync after delete 403",
          r["type"] == "msg.ack" and r["data"]["code"] == 403)
    cx.close()
    cy.close()

    # 用户 id 缓存自愈（相位无关版）：legacy 数字 userId 路径
    # （resolveTarget→exists→ids_ 纯缓存，不回源）。原"直插后立即发"
    # 版本依赖 purge 轮询相位（INSERT 期间一轮 sync 跨 COMMIT 即反转，
    # CI 慢机器偶发红）——改为双向确定性验证：
    #   ① 直插 + 等一轮 sync → 发送 0（缓存重建后命中，证明缓存路径在用）
    #   ② 删行 + 等一轮 sync → 发送 404（缓存与库重新对齐，自愈闭环）
    # 修复前：ids_ 缓存无自愈机制（联系人/群成员有 syncCaches，用户没有），
    # ②处将永远 0（缓存 stale 命中）——自愈缺失即红
    Z = base + "33"
    sql_q("INSERT INTO users (account, username, salt, password_hash) "
          "VALUES ('%s', '直插乙', 's', 'h')" % Z)
    zid = int(sql_q("SELECT id FROM users WHERE account='%s'" % Z))
    time.sleep(3)  # 等一轮缓存比对（测试 IM_OFFLINE_PURGE_INTERVAL=2）
    a.send(env("msg.send", {"to": zid, "content": "post-insert", "clientMsgId": "uc-a"}))
    r = json.loads(a.recv())
    check("user cache sync picks up inserted user",
          r["type"] == "msg.ack" and r["data"]["code"] == 0)
    sql_q("DELETE FROM users WHERE id=%d" % zid)
    time.sleep(3)  # 再等一轮：缓存重建（Z 行消失）
    a.send(env("msg.send", {"to": zid, "content": "post-delete", "clientMsgId": "uc-b"}))
    r = json.loads(a.recv())
    check("user cache sync heals after row deleted",
          r["type"] == "msg.ack" and r["data"]["code"] == 404)


    # ---- v2.4 拉黑（双向断联）----
    # BL 拉黑 BL2：消息双向 403（文案分方向）、好友请求双向拒（被拉黑方
    # 文案模糊防探测）、自动删联系人、群聊不受影响、历史不动、解除恢复。
    # 缓存自愈用相位无关双向验证（照 user cache 模式）
    BL = base + "34"
    BL2 = base + "35"
    bl = WS()
    r = register(bl, BL, "pw1234", nick="拉黑甲")
    blid = r["data"]["data"]["userId"]
    bl2 = WS()
    r = register(bl2, BL2, "pw1234", nick="拉黑乙")
    bl2id = r["data"]["data"]["userId"]
    r = login(bl2, BL2, "pw1234")
    # 成为好友 + 同群（群须在拉黑前建立：拉黑删联系人后无法邀请）
    r = login(bl, BL, "pw1234")
    bl.send(env("contact.request", {"account": BL2}))
    json.loads(bl.recv())  # request_res
    json.loads(bl2.recv())  # contact.request 推送
    bl2.send(env("contact.accept", {"account": BL}))
    json.loads(bl2.recv())  # accept_res
    json.loads(bl.recv())  # contact.accepted 推送
    bl.send(env("group.create", {"name": "拉黑群"}))
    r = json.loads(bl.recv())
    blgid = r["data"]["data"]["groupId"]
    bl.send(env("group.invite", {"groupId": blgid, "account": BL2}))
    json.loads(bl.recv())  # invite_res
    json.loads(bl2.recv())  # group.added 推送
    json.loads(bl.recv())  # member_changed 推送
    # 基线：BL2→BL 消息正常
    r = send_msg(bl2, BL, "before-block", "bb-1")
    check("block baseline send ok", r["data"]["code"] == 0)
    json.loads(bl.recv())  # bl 消费 msg.recv
    # 拉黑
    bl.send(env("block.add", {"account": BL2}))
    r = json.loads(bl.recv())
    check("block add ok", r["type"] == "block.add_res" and r["data"]["code"] == 0)
    # 拉黑方发：你已拉黑对方
    r = send_msg(bl, BL2, "blocked-try", "bb-2")
    check("blocker send 403", r["data"]["code"] == 403)
    # 被拉黑方发：对方已拒收（BL2 默认 allow_temp=1——403 即优先级证明）
    r = send_msg(bl2, BL, "blocked-try2", "bb-3")
    check("blocked send 403", r["data"]["code"] == 403)
    # 被拉黑方请求：模糊文案防探测（与 allowAdd 关同码同文案）
    bl2.send(env("contact.request", {"account": BL}))
    r = json.loads(bl2.recv())
    check("blocked request rejected fuzzy",
          r["type"] == "contact.request_res" and r["data"]["code"] == 403)
    # 拉黑方请求：你已拉黑对方
    bl.send(env("contact.request", {"account": BL2}))
    r = json.loads(bl.recv())
    check("blocker request 403", r["data"]["code"] == 403)
    # 自动删联系人：双方列表均无对方
    bl.send(env("contact.list", {}))
    r = json.loads(bl.recv())
    bl_accounts = [x["account"] for x in r["data"]["data"]["contacts"]]
    check("blocker contact list has no blocked",
          BL2 not in bl_accounts and BL in bl_accounts)
    bl2.send(env("contact.list", {}))
    r = json.loads(bl2.recv())
    bl2_accounts = [x["account"] for x in r["data"]["data"]["contacts"]]
    check("blocked contact list has no blocker", BL not in bl2_accounts)
    # block.list
    bl.send(env("block.list", {}))
    r = json.loads(bl.recv())
    blocks = r["data"]["data"]["blocks"]
    check("block list has entry",
          r["type"] == "block.list_res" and r["data"]["code"] == 0
          and len(blocks) == 1 and blocks[0]["account"] == BL2
          and blocks[0]["username"] == "拉黑乙")
    # 群聊不受影响：BL2 群发言 BL 照常收到
    bl.send(env("msg.send", {"to": blgid, "content": "group-while-blocked",
                             "chatType": "group", "clientMsgId": "bb-4"}))
    r = json.loads(bl.recv())
    check("block group send ok", r["data"]["code"] == 0)
    json.loads(bl2.recv())  # bl2 收群推送
    # 拉黑期间不落库
    nblk = sql_q("SELECT COUNT(*) FROM messages WHERE from_user=%d AND to_user=%d"
                 % (bl2id, blid))
    check("blocked send not persisted", nblk == "1")  # 仅拉黑前 bb-1 一条
    # 历史不动：拉黑前消息照常可查
    bl.send(env("chat.history", {"to": BL2}))
    r = json.loads(bl.recv())
    hist = [m["content"] for m in r["data"]["data"]["msgs"]]
    check("history untouched by block", "before-block" in hist)
    # 边界：拉黑自己 400 / 重复 409 / 解除未拉黑 409
    bl.send(env("block.add", {"account": BL}))
    r = json.loads(bl.recv())
    check("block self 400", r["data"]["code"] == 400)
    bl.send(env("block.add", {"account": BL2}))
    r = json.loads(bl.recv())
    check("block dup 409", r["data"]["code"] == 409)
    # 解除
    bl.send(env("block.remove", {"account": BL2}))
    r = json.loads(bl.recv())
    check("block remove ok", r["type"] == "block.remove_res" and r["data"]["code"] == 0)
    bl.send(env("block.remove", {"account": BL2}))
    r = json.loads(bl.recv())
    check("block remove again 409", r["data"]["code"] == 409)
    # 解除后通信恢复（非好友 + BL2 默认临时对话开 → 0）
    r = send_msg(bl2, BL, "after-unblock", "bb-5")
    check("unblock send restored", r["data"]["code"] == 0)
    json.loads(bl.recv())
    # 解除不恢复联系人：可重新申请
    bl.send(env("contact.request", {"account": BL2}))
    r = json.loads(bl.recv())
    check("unblock request can re-initiate", r["data"]["code"] == 0)
    json.loads(bl2.recv())  # bl2 消费 request 推送
    bl.close(); bl2.close()

    # 拉黑缓存自愈（相位无关双向，照 user cache 模式）：
    # 直插 blocks 行 + 等 sync → 403（生效）；直删行 + 等 sync → 0（自愈）
    BL3 = base + "36"
    BL4 = base + "37"
    bl3 = WS()
    r = register(bl3, BL3, "pw1234", nick="黑丙")
    bl3id = r["data"]["data"]["userId"]
    bl4 = WS()
    r = register(bl4, BL4, "pw1234", nick="黑丁")
    bl4id = r["data"]["data"]["userId"]
    r = login(bl3, BL3, "pw1234")
    sql_q("INSERT INTO blocks (user_id, blocked_id, created_ts) VALUES (%d, %d, %d)"
          % (bl3id, bl4id, int(time.time())))
    time.sleep(3)  # 等一轮缓存比对
    r = send_msg(bl3, BL4, "heal-probe-1", "bh-1")
    check("block cache sync picks up inserted row",
          r["data"]["code"] == 403)
    sql_q("DELETE FROM blocks WHERE user_id=%d AND blocked_id=%d" % (bl3id, bl4id))
    time.sleep(3)  # 再等一轮
    r = send_msg(bl3, BL4, "heal-probe-2", "bh-2")
    check("block cache sync heals after row deleted",
          r["data"]["code"] == 0)
    bl3.close(); bl4.close()

    # 唯一性防线：messages.message_id 唯一索引——自愈 INSERT 重试
    # （连接断时语句可能已执行）撞键被拒，绝不产生重复消息
    import subprocess as dup_probe
    mid = sql_q("SELECT message_id FROM messages LIMIT 1")
    dup_cmd = ("MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s -e \""
               "USE IMS_reactor_db; INSERT INTO messages (message_id, from_user, to_user,"
               " content, ts, client_msg_id, chat_type, group_id, status)"
               " SELECT message_id, from_user, to_user, 'dup-probe', 1, '', 'single', 0, 0"
               " FROM messages WHERE message_id='%s';\""
               % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, mid))
    dup_rc = dup_probe.run(["bash", "-c", dup_cmd], capture_output=True, text=True)
    check("message_id unique rejects duplicate insert", dup_rc.returncode != 0)

    # 可靠性：MySQL 连接被服务端踢掉后自动重连自愈。
    # 场景来自生产（2026-09-29）：闲置 19 小时 > wait_timeout(8h)，服务端
    # 踢连接（errno 4031/2006），单连接无自愈导致登录全挂直到重启。
    # 此处用 KILL 模拟连接被踢，验证下一次查询自动重建并成功
    kill = sql_q("SELECT GROUP_CONCAT(id) FROM information_schema.processlist "
                 "WHERE user='IMS_jack' AND command='Sleep' AND id <> CONNECTION_ID()")
    n_killed = 0
    for kid in (kill or "").split(","):
        kid = kid.strip()
        if kid.isdigit():
            sql_q("KILL %s" % kid)
            n_killed += 1
    check("mysql conn kick executed", n_killed >= 1)
    time.sleep(0.5)  # 等 TCP 断开传播到服务端
    # 登录换干净账号 X（缓存用例注册且仅成功登录过——A 在前段用例里
    # 有故意失败尝试，guard 计数未过期会 429，与本用例无关）
    c5 = WS()
    r = login(c5, X, "pw1234")
    check("login after mysql kick self-heals",
          r["type"] == "auth.login_res" and r["data"]["code"] == 0)
    # 发消息走 msgRepo 落库自愈：发给 A 自己（自己对话框跳权限、
    # 只落库不推送——避免给已关闭的 b 产生 pending 污染后续 B 重登）
    r = send_msg(a, A, "post-kick", "mysql-kick-1")
    check("send after mysql kick self-heals",
          r["type"] == "msg.ack" and r["data"]["code"] == 0)
    # 联系人列表走 contactRepo 查询自愈
    c5.send(env("contact.list", {}))
    r = json.loads(c5.recv())
    check("contact list after kick self-heals",
          r["type"] == "contact.list_res" and r["data"]["code"] == 0)
    c5.close()

    # 可靠性二期：MySQL 整体宕机 → 期间查询失败（自愈重连也不可达）→
    # 恢复后服务必须自动跟上（不因连接状态被关门而瘫痪到重启）。
    # 需本机 docker 控制 mysql 容器；CI（GitHub services mysql）无 docker 跳过
    import shutil
    import subprocess as sp_mod
    if shutil.which("docker"):
        probe = sp_mod.run(["docker", "inspect", "-f", "1", "ims-mysql"],
                           capture_output=True)
        if probe.returncode == 0:
            sp_mod.run(["docker", "stop", "ims-mysql"], capture_output=True)
            time.sleep(1)
            r = send_msg(a, A, "during-outage", "mysql-outage-1")
            check("send during mysql outage fails gracefully",
                  r["type"] == "msg.ack" and r["data"]["code"] == 500)
            sp_mod.run(["docker", "start", "ims-mysql"], capture_output=True)
            time.sleep(15)  # 等 MySQL 完全就绪（启动+网络）
            r = send_msg(a, A, "after-recovery", "mysql-outage-2")
            check("send after mysql recovery auto-resumes",
                  r["type"] == "msg.ack" and r["data"]["code"] == 0)
        else:
            print("SKIP mysql outage/recovery probe (ims-mysql not a local container)")
    else:
        print("SKIP mysql outage/recovery probe (no docker)")

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
    # v2.1：联系人列表下发在线状态（Presence，内存/Redis 双后端）
    d.send(env("contact.list", {}))
    r = json.loads(d.recv())
    emap = {x["account"]: x for x in r["data"]["data"]["contacts"]}
    check("contact online E true", emap.get(E, {}).get("online") is True)
    check("contact online self true", emap.get(D, {}).get("online") is True)
    # 成为联系人后互发消息成功
    r = send_msg(d, E, "hi contact", "ct1")
    check("contact send ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    r = json.loads(e.recv())
    check("contact recv ok", r["type"] == "msg.recv" and r["data"]["from"] == D)

    # v2.2：多端一致性——D 第二端登录；d 发消息，d2 收到自发回显；
    # d 已读，d2 收到 msg.read_notify
    d2 = WS()
    login(d2, D, "pw1234")
    r = send_msg(d, E, "echo probe", "md1")
    check("multi-device send ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    json.loads(e.recv())  # e 消费 msg.recv（接收方）
    r = json.loads(d2.recv())
    check("self echo to other device", r["type"] == "msg.recv"
          and r["data"]["from"] == D and r["data"]["content"] == "echo probe"
          and r["data"]["chatType"] == "single")
    d.send(env("msg.read", {"from": E}))
    r = json.loads(d.recv())
    check("multi-device read ok", r["type"] == "msg.read_res" and r["data"]["code"] == 0)
    r = json.loads(d2.recv())
    check("read notify to other device", r["type"] == "msg.read_notify"
          and r["data"]["from"] == E and "readTs" in r["data"])
    # v2.2.1 收紧：与 D 无任何消息往来的目标（数字 userId）不应触发 read_notify
    # （防止双端自收回执把 userId 枚举成账号，穿透 searchable=0）
    d.send(env("msg.read", {"from": sink_id}))
    r = json.loads(d.recv())
    check("read no-relation ok", r["type"] == "msg.read_res" and r["data"]["code"] == 0)
    try:
        leaked = d2.recv(0.5)
    except socket.timeout:
        leaked = None
    check("read notify not leaked for stranger", leaked is None)
    d2.close()

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

    # v2.1：Presence 离线翻转（D-E 已建联，d 在线观察 e）
    e.close()
    time.sleep(1)
    d.send(env("contact.list", {}))
    r = json.loads(d.recv())
    dmap = {x["account"]: x for x in r["data"]["data"]["contacts"]}
    check("presence offline flip", dmap.get(E, {}).get("online") is False)
    e2 = WS()
    login(e2, E, "pw1234")
    d.send(env("contact.list", {}))
    r = json.loads(d.recv())
    dmap = {x["account"]: x for x in r["data"]["data"]["contacts"]}
    check("presence online flip", dmap.get(E, {}).get("online") is True)
    e2.close()
    time.sleep(1)
    d.send(env("contact.list", {}))
    r = json.loads(d.recv())
    dmap = {x["account"]: x for x in r["data"]["data"]["contacts"]}
    check("presence offline flip again", dmap.get(E, {}).get("online") is False)
    d.close(); f.close()

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
    # 任务 10：群聊图片消息改走对象存储直传（全员收 mediaId，历史带 mediaId）
    # 环境门禁与单聊一致：未配置对象存储时显式跳过，不中断后续用例。
    if medias_enabled:
      png_g = base64.b64decode(PNG_1X1_B64)
      r = send(a, "media.presign", {"fileName": "g.png", "mimeType": "image/png",
                                    "size": len(png_g), "kind": "image"})
      check("group image presign ok",
            r["type"] == "media.presign_res" and media_ok(r))
      g_up = media_data(r).get("uploadUrl", "")
      g_fid = media_data(r).get("fileId", "")
      check("group image PUT ok", bool(g_up) and put(g_up, png_g) in (200, 204))
      r = send(a, "media.commit", {"fileId": g_fid, "size": len(png_g), "width": 1, "height": 1})
      check("group image commit ok",
            r["type"] == "media.commit_res" and r["data"]["code"] == 0)
      a.send(env("msg.send", {"to": gid, "content": "", "clientMsgId": "g-img-1",
                              "chatType": "group", "msgType": "image", "mediaId": g_fid}))
      r = json.loads(a.recv())
      check("group image send ack ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
      r = json.loads(b.recv())
      check("group image recv mediaId", r["type"] == "msg.recv"
            and r["data"].get("mediaId") == g_fid and r["data"].get("msgType") == "image")
      # 群成员 B 取 GET URL → 字节一致
      r = send(b, "media.url", {"fileId": g_fid})
      gent = media_data(r).get("urls", {}).get(g_fid, {})
      check("group image member url ok",
            "url" in gent and get(gent["url"]) == png_g)
      # 负例：群聊图片非成员取 url 拒绝（g1 从未加入 → 403）
      r = send(g1, "media.url", {"fileId": g_fid})
      check("group image non-member url 403",
            media_data(r).get("urls", {}).get(g_fid, {}).get("code") == 403)
      b.send(env("group.history", {"groupId": gid}))
      r = json.loads(b.recv())
      gimgs = [m for m in r["data"]["data"]["msgs"] if m.get("msgType") == "image"]
      check("group image history has mediaId", r["type"] == "group.history_res"
            and r["data"]["code"] == 0 and len(gimgs) >= 1
            and all(m.get("mediaId") == g_fid for m in gimgs))
    else:
      skip("media group suite", "IM_S3_ENDPOINT not set (object store disabled)")
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
    # v2.1：群成员列表下发在线状态
    mmap = {x["account"]: x for x in r["data"]["data"]["members"]}
    check("group members online true", all(m.get("online") is True
                                           for m in mmap.values()))
    # v2.3：重查询限流——连续拉群历史超 20 次/10s 触发 429
    # （G1 曾被拒入群，用 group.history 对不存在群拉取也会先消耗查询额度）
    h = WS(); login(h, G1, "pw1234")
    got_429 = False
    for i in range(25):
        h.send(env("group.history", {"groupId": gid}))
        r = json.loads(h.recv())
        if r["type"] == "group.history_res" and r["data"]["code"] == 429:
            got_429 = True
            break
    check("query rate limit 429", got_429)
    h.close()

    # v2.2：群自发回显到其他端——A 第二端登录，a 群发，a3 收到、发起端仍不回显
    # token 重连做第二端（A 的登录已被错密码用例限流锁定，login 会 429）
    a3 = WS()
    a3.send(env("auth.token", {"token": alice_token}))
    lr3 = json.loads(a3.recv())
    a.send(env("msg.send", {"to": gid, "content": "grp echo", "clientMsgId": "g-echo",
                            "chatType": "group"}))
    r = json.loads(a.recv())
    check("group echo send ok", r["type"] == "msg.ack" and r["data"]["code"] == 0)
    json.loads(b.recv())  # b 消费群消息
    r = json.loads(a3.recv())
    check("group self echo to other device", r["type"] == "msg.recv"
          and r["data"]["from"] == A and r["data"]["groupId"] == gid
          and r["data"]["content"] == "grp echo" and r["data"]["chatType"] == "group")
    a3.close()
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
    # 重入语义：退群成员由群主重新拉入 → 新发言可见、旧时段消息不可见
    # （"hello group" 为退群前旧消息，"after-rejoin" 为重入后新消息）
    time.sleep(1.1)  # 跨秒：重入 joined 须大于旧消息 ts（history 按 joined-1 严格大于过滤）
    a.send(env("group.invite", {"groupId": gid, "account": B}))
    r = json.loads(a.recv())
    check("group re-invite ok", r["type"] == "group.invite_res"
          and r["data"]["code"] == 0)
    r = json.loads(b.recv())
    check("group re-added pushed", r["type"] == "group.added"
          and r["data"]["groupId"] == gid)
    json.loads(a.recv())  # 消费 group.member_changed
    a.send(env("msg.send", {"to": gid, "content": "after-rejoin", "clientMsgId": "g5",
                            "chatType": "group"}))
    r = json.loads(a.recv())
    check("group send after rejoin ack ok", r["type"] == "msg.ack"
          and r["data"]["code"] == 0)
    r = json.loads(b.recv())
    check("group recv after rejoin ok", r["type"] == "msg.recv"
          and r["data"]["content"] == "after-rejoin")
    b.send(env("group.history", {"groupId": gid}))
    r = json.loads(b.recv())
    rejoin_contents = [m["content"] for m in r["data"]["data"]["msgs"]]
    check("group rejoin sees new", r["data"]["code"] == 0
          and "after-rejoin" in rejoin_contents)
    check("group rejoin not sees old", "hello group" not in rejoin_contents)
    # 50 人上限：新群 + 49 新成员 = 50 人已满，第 51 人 invite → 403 群已满
    a.send(env("group.create", {"name": "满员群"}))
    r = json.loads(a.recv())
    check("group cap create ok", r["type"] == "group.create_res"
          and r["data"]["code"] == 0)
    gid2 = r["data"]["data"]["groupId"]
    # 11 位合规账号：base(9 位)+2 位后缀，跳过已用后缀避免冲突
    cap_block = {"10", "11", "12", "13", "14", "15", "31", "32", "33", "34", "35", "36", "37", "50", "60", "61", "77"}
    cap_accounts = []
    n = 16
    while len(cap_accounts) < 49:
        cand = base + "%02d" % n
        n += 1
        if cand[len(base):] in cap_block:
            continue
        cap_accounts.append(cand)
    cap_ok = True
    for i, acct in enumerate(cap_accounts):
        u = WS()
        rr = register(u, acct, "pw1234")
        assert rr["data"]["code"] == 0, rr
        rr = login(u, acct, "pw1234")
        assert rr["data"]["code"] == 0, rr
        u.send(env("contact.request", {"account": A}))
        json.loads(u.recv())      # request_res
        json.loads(a.recv())      # contact.request 推送
        a.send(env("contact.accept", {"account": acct}))
        json.loads(a.recv())      # accept_res
        json.loads(u.recv())      # contact.added
        a.send(env("group.invite", {"groupId": gid2, "account": acct}))
        r = json.loads(a.recv())
        if r["type"] == "group.invite_res" and r["data"]["code"] == 0:
            json.loads(u.recv())  # group.added
            json.loads(a.recv())  # group.member_changed
        else:
            cap_ok = False
        u.close()
    check("group cap 49 invites ok", cap_ok)
    a.send(env("group.members", {"groupId": gid2}))
    r = json.loads(a.recv())
    check("group cap members 50", r["type"] == "group.members_res"
          and r["data"]["code"] == 0
          and len(r["data"]["data"]["members"]) == 50)
    # 第 51 人（第 50 个新账号）：先建联再 invite → 403 群已满
    OVER = base + "78"
    o = WS(); register(o, OVER, "pw1234"); login(o, OVER, "pw1234")
    o.send(env("contact.request", {"account": A}))
    json.loads(o.recv()); json.loads(a.recv())
    a.send(env("contact.accept", {"account": OVER}))
    json.loads(a.recv()); json.loads(o.recv())
    a.send(env("group.invite", {"groupId": gid2, "account": OVER}))
    r = json.loads(a.recv())
    check("group invite over cap 403", r["type"] == "group.invite_res"
          and r["data"]["code"] == 403)
    o.close()

    # ---- v2.0 群管理（踢/转让/解散/改名/公告）----
    # 新账号段 80~83（11 位合规；cap 占用 16~67、OVER 用 78，故选 80 段避碰撞）
    M1 = base + "80"; M2 = base + "81"; M3 = base + "82"; M4 = base + "83"
    m1 = WS(); register(m1, M1, "pw1234"); login(m1, M1, "pw1234")
    m2 = WS(); register(m2, M2, "pw1234"); login(m2, M2, "pw1234")
    m3 = WS(); register(m3, M3, "pw1234"); login(m3, M3, "pw1234")
    m4 = WS(); register(m4, M4, "pw1234"); login(m4, M4, "pw1234")

    def _befriend(x, xacct, y, yacct):
        # 两账号建联（request+accept，消费双方推送）
        x.send(env("contact.request", {"account": yacct}))
        json.loads(x.recv()); json.loads(y.recv())
        y.send(env("contact.accept", {"account": xacct}))
        json.loads(y.recv()); json.loads(x.recv())
    _befriend(m1, M1, m2, M2)
    _befriend(m1, M1, m3, M3)
    _befriend(m2, M2, m3, M3)

    # 踢人组：M1 群主，M2/M3 成员，M4 为圈外人（存在但非成员）
    m1.send(env("group.create", {"name": "踢人组"}))
    r = json.loads(m1.recv())
    check("mgmt kick create ok", r["type"] == "group.create_res" and r["data"]["code"] == 0)
    kgid = r["data"]["data"]["groupId"]
    m1.send(env("group.invite", {"groupId": kgid, "account": M2}))
    check("mgmt kick invite M2 ok", json.loads(m1.recv())["data"]["code"] == 0)
    json.loads(m2.recv()); json.loads(m1.recv())
    m1.send(env("group.invite", {"groupId": kgid, "account": M3}))
    check("mgmt kick invite M3 ok", json.loads(m1.recv())["data"]["code"] == 0)
    json.loads(m3.recv()); json.loads(m1.recv()); json.loads(m2.recv())
    # 踢前旧消息（重拉后应不可见）
    m1.send(env("msg.send", {"to": kgid, "content": "kick-old", "clientMsgId": "kk0",
                             "chatType": "group"}))
    check("mgmt kick-old ack ok", json.loads(m1.recv())["data"]["code"] == 0)
    json.loads(m2.recv()); json.loads(m3.recv())
    # 群主踢成员 ok；被踢者收 removed，其余成员收 kick 事件
    m1.send(env("group.kick", {"groupId": kgid, "account": M2}))
    r = json.loads(m1.recv())
    check("mgmt kick ok", r["type"] == "group.kick_res" and r["data"]["code"] == 0)
    r = json.loads(m2.recv())
    check("mgmt kick removed pushed", r["type"] == "group.removed"
          and r["data"]["groupId"] == kgid)
    r = json.loads(m1.recv())
    check("mgmt kick member_changed", r["type"] == "group.member_changed"
          and r["data"]["groupId"] == kgid and r["data"]["change"] == "kick"
          and r["data"]["account"] == M2)
    json.loads(m3.recv())  # 消费 m3 的 kick 事件
    # 被踢者群发 403
    m2.send(env("msg.send", {"to": kgid, "content": "x", "clientMsgId": "kk1",
                             "chatType": "group"}))
    r = json.loads(m2.recv())
    check("mgmt kicked send 403", r["type"] == "msg.ack" and r["data"]["code"] == 403)
    # 踢人权限：非群主 403、踢群主本人 409、踢非成员 409、目标不存在 404
    m3.send(env("group.kick", {"groupId": kgid, "account": M1}))
    r = json.loads(m3.recv())
    check("mgmt kick non-owner 403", r["type"] == "group.kick_res"
          and r["data"]["code"] == 403)
    m1.send(env("group.kick", {"groupId": kgid, "account": M1}))
    r = json.loads(m1.recv())
    check("mgmt kick owner-self 409", r["type"] == "group.kick_res"
          and r["data"]["code"] == 409)
    m1.send(env("group.kick", {"groupId": kgid, "account": M4}))
    r = json.loads(m1.recv())
    check("mgmt kick non-member 409", r["type"] == "group.kick_res"
          and r["data"]["code"] == 409)
    m1.send(env("group.kick", {"groupId": kgid, "account": NOBODY}))
    r = json.loads(m1.recv())
    check("mgmt kick missing 404", r["type"] == "group.kick_res"
          and r["data"]["code"] == 404)
    # 重拉：跨秒保证新窗口，重拉后新发言可见、旧消息不可见
    time.sleep(1.1)
    m1.send(env("group.invite", {"groupId": kgid, "account": M2}))
    check("mgmt kick re-invite ok", json.loads(m1.recv())["data"]["code"] == 0)
    json.loads(m2.recv()); json.loads(m1.recv()); json.loads(m3.recv())
    m1.send(env("msg.send", {"to": kgid, "content": "kick-new", "clientMsgId": "kk2",
                             "chatType": "group"}))
    check("mgmt kick-new ack ok", json.loads(m1.recv())["data"]["code"] == 0)
    json.loads(m2.recv()); json.loads(m3.recv())
    m2.send(env("group.history", {"groupId": kgid}))
    r = json.loads(m2.recv())
    contents = [x["content"] for x in r["data"]["data"]["msgs"]]
    check("mgmt kick rejoin sees new", r["data"]["code"] == 0 and "kick-new" in contents)
    check("mgmt kick rejoin not sees old", "kick-old" not in contents)

    # 转让组：M1 群主转给 M2
    m1.send(env("group.create", {"name": "转让组"}))
    tgid = json.loads(m1.recv())["data"]["data"]["groupId"]
    m1.send(env("group.invite", {"groupId": tgid, "account": M2}))
    json.loads(m1.recv()); json.loads(m2.recv()); json.loads(m1.recv())
    m1.send(env("group.transfer", {"groupId": tgid, "account": M2}))
    r = json.loads(m1.recv())
    check("mgmt transfer ok", r["type"] == "group.transfer_res" and r["data"]["code"] == 0)
    r = json.loads(m2.recv())
    check("mgmt owner_changed pushed", r["type"] == "group.owner_changed"
          and r["data"]["groupId"] == tgid and r["data"]["account"] == M2)
    json.loads(m1.recv())  # 消费 m1 的 owner_changed
    # 原群主变为成员后可退群
    m1.send(env("group.leave", {"groupId": tgid}))
    check("mgmt ex-owner leave ok", json.loads(m1.recv())["data"]["code"] == 0)
    json.loads(m2.recv())  # 消费 leave 的 member_changed
    # 新群主可拉人（M2 与 M3 已建联）
    m2.send(env("group.invite", {"groupId": tgid, "account": M3}))
    check("mgmt new-owner invite ok", json.loads(m2.recv())["data"]["code"] == 0)
    json.loads(m3.recv()); json.loads(m2.recv())
    # 转让权限：非群主 403、转非成员 409、目标不存在 404、转自己 409
    m3.send(env("group.transfer", {"groupId": tgid, "account": M3}))
    r = json.loads(m3.recv())
    check("mgmt transfer non-owner 403", r["type"] == "group.transfer_res"
          and r["data"]["code"] == 403)
    m2.send(env("group.transfer", {"groupId": tgid, "account": M4}))
    r = json.loads(m2.recv())
    check("mgmt transfer non-member 409", r["type"] == "group.transfer_res"
          and r["data"]["code"] == 409)
    m2.send(env("group.transfer", {"groupId": tgid, "account": NOBODY}))
    r = json.loads(m2.recv())
    check("mgmt transfer missing 404", r["type"] == "group.transfer_res"
          and r["data"]["code"] == 404)
    m2.send(env("group.transfer", {"groupId": tgid, "account": M2}))
    r = json.loads(m2.recv())
    check("mgmt transfer self 409", r["type"] == "group.transfer_res"
          and r["data"]["code"] == 409)

    # 解散组：M1 群主，M2 成员
    m1.send(env("group.create", {"name": "解散组"}))
    dgid = json.loads(m1.recv())["data"]["data"]["groupId"]
    m1.send(env("group.invite", {"groupId": dgid, "account": M2}))
    json.loads(m1.recv()); json.loads(m2.recv()); json.loads(m1.recv())
    m1.send(env("msg.send", {"to": dgid, "content": "bye-group", "clientMsgId": "dd0",
                             "chatType": "group"}))
    json.loads(m1.recv()); json.loads(m2.recv())
    # 非群主解散 403（先验权限，组仍存活）
    m2.send(env("group.dissolve", {"groupId": dgid}))
    r = json.loads(m2.recv())
    check("mgmt dissolve non-owner 403", r["type"] == "group.dissolve_res"
          and r["data"]["code"] == 403)
    m1.send(env("group.dissolve", {"groupId": dgid}))
    r = json.loads(m1.recv())
    check("mgmt dissolve ok", r["type"] == "group.dissolve_res" and r["data"]["code"] == 0)
    r = json.loads(m1.recv())
    check("mgmt dissolved pushed self", r["type"] == "group.dissolved"
          and r["data"]["groupId"] == dgid)
    r = json.loads(m2.recv())
    check("mgmt dissolved pushed member", r["type"] == "group.dissolved"
          and r["data"]["groupId"] == dgid)
    # list 不再列出
    m1.send(env("group.list", {}))
    r = json.loads(m1.recv())
    check("mgmt dissolve list filtered",
          r["data"]["code"] == 0
          and all(x["groupId"] != dgid for x in r["data"]["data"]["groups"]))
    # 解散后操作 404：群发 / 拉人 / 成员 / 改名 / 公告 / 踢人
    m1.send(env("msg.send", {"to": dgid, "content": "x", "clientMsgId": "dd1",
                             "chatType": "group"}))
    r = json.loads(m1.recv())
    check("mgmt dissolved send 404", r["type"] == "msg.ack" and r["data"]["code"] == 404)
    m1.send(env("group.invite", {"groupId": dgid, "account": M3}))
    r = json.loads(m1.recv())
    check("mgmt dissolved invite 404", r["type"] == "group.invite_res"
          and r["data"]["code"] == 404)
    m1.send(env("group.members", {"groupId": dgid}))
    r = json.loads(m1.recv())
    check("mgmt dissolved members 404", r["type"] == "group.members_res"
          and r["data"]["code"] == 404)
    m1.send(env("group.kick", {"groupId": dgid, "account": M2}))
    r = json.loads(m1.recv())
    check("mgmt kick dissolved 404", r["type"] == "group.kick_res"
          and r["data"]["code"] == 404)
    m1.send(env("group.rename", {"groupId": dgid, "name": "x"}))
    r = json.loads(m1.recv())
    check("mgmt rename dissolved 404", r["type"] == "group.rename_res"
          and r["data"]["code"] == 404)
    m1.send(env("group.announce", {"groupId": dgid, "content": "x"}))
    r = json.loads(m1.recv())
    check("mgmt announce dissolved 404", r["type"] == "group.announce_res"
          and r["data"]["code"] == 404)
    m1.send(env("group.transfer", {"groupId": dgid, "account": M2}))
    r = json.loads(m1.recv())
    check("mgmt transfer dissolved 404", r["type"] == "group.transfer_res"
          and r["data"]["code"] == 404)
    # 曾成员仍可查解散前窗口历史
    m2.send(env("group.history", {"groupId": dgid}))
    r = json.loads(m2.recv())
    check("mgmt dissolved history ok", r["type"] == "group.history_res"
          and r["data"]["code"] == 0
          and any(x["content"] == "bye-group" for x in r["data"]["data"]["msgs"]))

    # 改名/公告组：M1 群主，M2 成员
    m1.send(env("group.create", {"name": "改名组"}))
    rgid = json.loads(m1.recv())["data"]["data"]["groupId"]
    m1.send(env("group.invite", {"groupId": rgid, "account": M2}))
    json.loads(m1.recv()); json.loads(m2.recv()); json.loads(m1.recv())
    m1.send(env("group.rename", {"groupId": rgid, "name": "新群名"}))
    r = json.loads(m1.recv())
    check("mgmt rename ok", r["type"] == "group.rename_res" and r["data"]["code"] == 0)
    r = json.loads(m1.recv())
    check("mgmt renamed pushed self", r["type"] == "group.renamed"
          and r["data"]["groupId"] == rgid and r["data"]["name"] == "新群名")
    r = json.loads(m2.recv())
    check("mgmt renamed pushed member", r["type"] == "group.renamed"
          and r["data"]["name"] == "新群名")
    m1.send(env("group.rename", {"groupId": rgid, "name": ""}))
    r = json.loads(m1.recv())
    check("mgmt rename empty 400", r["type"] == "group.rename_res"
          and r["data"]["code"] == 400)
    m1.send(env("group.rename", {"groupId": rgid, "name": "a" * 33}))
    r = json.loads(m1.recv())
    check("mgmt rename long 400", r["type"] == "group.rename_res"
          and r["data"]["code"] == 400)
    m2.send(env("group.rename", {"groupId": rgid, "name": "黑名"}))
    r = json.loads(m2.recv())
    check("mgmt rename non-owner 403", r["type"] == "group.rename_res"
          and r["data"]["code"] == 403)
    # 公告：发布 ok + 事件 + list 下发 + 清空 + 超限 + 非群主
    m1.send(env("group.announce", {"groupId": rgid, "content": "公告正文"}))
    r = json.loads(m1.recv())
    check("mgmt announce ok", r["type"] == "group.announce_res" and r["data"]["code"] == 0)
    check("mgmt announced pushed self",
          json.loads(m1.recv())["type"] == "group.announced")
    check("mgmt announced pushed member",
          json.loads(m2.recv())["type"] == "group.announced")
    m1.send(env("group.list", {}))
    r = json.loads(m1.recv())
    entry = [x for x in r["data"]["data"]["groups"] if x["groupId"] == rgid]
    check("mgmt announce list has body", len(entry) == 1
          and entry[0]["announcement"] == "公告正文")
    m1.send(env("group.announce", {"groupId": rgid, "content": ""}))
    check("mgmt announce clear ok", json.loads(m1.recv())["data"]["code"] == 0)
    json.loads(m1.recv()); json.loads(m2.recv())  # 消费清空事件
    m1.send(env("group.list", {}))
    r = json.loads(m1.recv())
    entry = [x for x in r["data"]["data"]["groups"] if x["groupId"] == rgid]
    check("mgmt announce cleared", len(entry) == 1 and entry[0]["announcement"] == "")
    m1.send(env("group.announce", {"groupId": rgid, "content": "x" * 1025}))
    r = json.loads(m1.recv())
    check("mgmt announce oversize 413", r["type"] == "group.announce_res"
          and r["data"]["code"] == 413)
    m2.send(env("group.announce", {"groupId": rgid, "content": "黑公告"}))
    r = json.loads(m2.recv())
    check("mgmt announce non-owner 403", r["type"] == "group.announce_res"
          and r["data"]["code"] == 403)
    m1.close(); m2.close(); m3.close(); m4.close()
    g1.close(); g2.close()

    # ================= 消息撤回（msg.recall）=================
    # 独立账号/连接，避免污染前段会话状态；DB 注入仅用于超窗用例。
    RA, RB, RC = base + "87", base + "88", base + "89"  # 16..74 被群满员动态注册占用,77-86 静态占用
    r1 = WS()   # 发送者主连接
    r1x = WS()  # 发送者第二连接（多端同步）
    rb = WS()   # 接收者
    rc = WS()   # 离线接收者（用后即关）
    assert register(r1, RA, "pw1234")["data"]["code"] == 0
    assert login(r1, RA, "pw1234")["data"]["code"] == 0
    # r1x 是 RA 的第二连接（多端同步）：仅登录，重复注册会 409
    assert login(r1x, RA, "pw1234")["data"]["code"] == 0
    assert register(rb, RB, "pw1234")["data"]["code"] == 0
    assert login(rb, RB, "pw1234")["data"]["code"] == 0
    assert register(rc, RC, "pw1234")["data"]["code"] == 0
    # RC 须先登录才可 accept（未登录信封门禁 401），建联后断开转离线
    assert login(rc, RC, "pw1234")["data"]["code"] == 0
    # 联系人：RA↔RB、RA↔RC（send 门槛）
    r1.send(env("contact.request", {"account": RB})); json.loads(r1.recv())
    rb.send(env("contact.accept", {"account": RA})); json.loads(rb.recv())
    json.loads(r1.recv()); json.loads(rb.recv())  # 双方 contact 事件
    r1.send(env("contact.request", {"account": RC})); json.loads(r1.recv())
    rc.send(env("contact.accept", {"account": RA})); json.loads(rc.recv())
    json.loads(r1.recv()); json.loads(rc.recv())
    json.loads(r1x.recv()); json.loads(r1x.recv())  # r1x 多端扇出：两条 contact.added
    rc.close()  # RC 转离线

    # 1. 单聊撤回全链路：recv → recall → 双端 notify → 历史占位符
    cid1 = "rc1-" + base
    r = send_msg(r1, RB, "撤回我", cid1)
    check("recall send ok", r["data"]["code"] == 0)
    r = json.loads(rb.recv())
    check("recall recv ok", r["type"] == "msg.recv" and r["data"]["clientMsgId"] == cid1)
    rid1 = r["data"]["messageId"]
    r = json.loads(r1x.recv())  # 发送者多端回显
    rid1x = r["data"]["messageId"]
    check("recall echo same id", rid1x == rid1)
    r = send(r1, "msg.recall", {"messageId": rid1})
    check("recall ok", r["type"] == "msg.recall_res" and r["data"]["code"] == 0)
    r = json.loads(rb.recv())
    check("recall notify to peer", r["type"] == "msg.recall_notify"
          and r["data"]["messageId"] == rid1 and r["data"]["chatType"] == "single"
          and r["data"]["from"] == RA)
    r = json.loads(r1x.recv())
    check("recall notify to other device", r["type"] == "msg.recall_notify"
          and r["data"]["messageId"] == rid1)
    r = send(rb, "chat.history", {"to": RA, "limit": 10})
    msgs = r["data"]["data"]["msgs"]
    hit = [m for m in msgs if m["messageId"] == rid1]
    check("recall history placeholder", len(hit) == 1 and hit[0].get("recalled") is True
          and hit[0]["content"] == "" and hit[0]["mediaId"] == "")

    # 2. 群聊撤回：群发 → 发送者撤回 → 成员 notify + 群历史占位符
    r = send(r1, "group.create", {"name": "撤回群"})
    rgid = r["data"]["data"]["groupId"]
    r = send(r1, "group.invite", {"groupId": rgid, "account": RB})
    json.loads(r1.recv())  # member_changed 事件
    json.loads(rb.recv())  # 被拉人 group.added 推送
    json.loads(r1x.recv())  # r1x 同为成员端，亦收 member_changed
    r = send_msg(r1, rgid, "群撤回我", "rcg-" + base, chat_type="group")
    check("recall group send ok", r["data"]["code"] == 0)
    r = json.loads(rb.recv())
    rgid_msg = r["data"]["messageId"]
    json.loads(r1x.recv())  # 群回显
    r = send(r1, "msg.recall", {"messageId": rgid_msg})
    check("recall group ok", r["data"]["code"] == 0)
    r = json.loads(rb.recv())
    check("recall group notify", r["type"] == "msg.recall_notify"
          and r["data"]["groupId"] == rgid and r["data"]["chatType"] == "group")
    r = send(rb, "group.history", {"groupId": rgid, "limit": 10})
    ghits = [m for m in r["data"]["data"]["msgs"] if m["messageId"] == rgid_msg]
    check("recall group history placeholder", len(ghits) == 1
          and ghits[0].get("recalled") is True and ghits[0]["content"] == "")

    # 3. 非发送者撤回 403
    r = send_msg(r1, RB, "别撤我的", "rc2-" + base)
    r = json.loads(rb.recv())
    rid2 = r["data"]["messageId"]
    json.loads(r1x.recv())
    r = send(rb, "msg.recall", {"messageId": rid2})
    check("recall non-sender 403", r["data"]["code"] == 403)

    # 4. 不存在 404
    r = send(r1, "msg.recall", {"messageId": "9999999999-999999"})
    check("recall not found 404", r["data"]["code"] == 404)

    # 5. 参数错误 400
    r = send(r1, "msg.recall", {})
    check("recall bad params 400", r["data"]["code"] == 400)

    # 6. 重复撤回幂等 0（对 rid2 发送者再撤一次）
    r = send(r1, "msg.recall", {"messageId": rid2})
    check("recall first ok", r["data"]["code"] == 0)
    json.loads(rb.recv()); json.loads(r1x.recv())  # 消费首次 notify
    r = send(r1, "msg.recall", {"messageId": rid2})
    check("recall twice idempotent 0", r["data"]["code"] == 0)

    # 7. 离线 pending 撤回：RC 不上线则永不收到该消息
    r = send_msg(r1, RC, "离线撤回", "rc3-" + base)
    check("recall offline send ok", r["data"]["code"] == 0)
    # msg.ack 不带 messageId：按 client_msg_id 从 DB 反查（见简报实现注意块）
    import subprocess
    out = subprocess.run(["bash", "-c",
        "MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s IMS_reactor_db -N "
        "-e \"SELECT message_id FROM messages WHERE client_msg_id='rc3-%s';\""
        % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, base)],
        capture_output=True, text=True).stdout.strip()
    r = send(r1, "msg.recall", {"messageId": out})
    check("recall offline pending ok", r["data"]["code"] == 0)
    r3 = WS()
    assert login(r3, RC, "pw1234")["data"]["code"] == 0
    got_offline = None
    try:
        r3.s.settimeout(2)
        got_offline = json.loads(r3.recv())
    except Exception:
        got_offline = None
    r3.s.settimeout(5)
    if got_offline is not None and got_offline["type"] == "msg.offline":
        mids = [m["messageId"] for m in got_offline["data"]["msgs"]]
        check("recall suppresses offline", out not in mids)
    else:
        check("recall suppresses offline", True)  # 无 offline 包 = 也通过
    r = send(r3, "chat.history", {"to": RA, "limit": 10})
    phits = [m for m in r["data"]["data"]["msgs"] if m["messageId"] == out]
    check("recall offline history placeholder", len(phits) == 1
          and phits[0].get("recalled") is True)

    # 8. 撤回后 markRead 不复活（回归：markRead 谓词 status IN(0,1)）
    #    （用例 1 已撤回 rid1；此处 RB 对 RA 全量已读后再查历史）
    r = send(rb, "msg.read", {"from": RA})
    try:
        rb.recv(0.5)  # 读通知仅发读者其他端（本端无推送），容忍性消费防阻塞
    except Exception:
        pass
    r = send(rb, "chat.history", {"to": RA, "limit": 10})
    hit2 = [m for m in r["data"]["data"]["msgs"] if m["messageId"] == rid1]
    check("recall survives markRead", len(hit2) == 1 and hit2[0].get("recalled") is True
          and hit2[0]["content"] == "")

    # 9. 超窗 410（DB 把 rid2 的 ts 改老后撤回）
    #    注意：rid2 在用例 6 已撤回，这里改用新消息
    r = send_msg(r1, RB, "超窗撤回", "rc4-" + base)
    r = json.loads(rb.recv())
    rid4 = r["data"]["messageId"]
    json.loads(r1x.recv())
    subprocess.run(["bash", "-c",
        "MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s IMS_reactor_db -e "
        "\"UPDATE messages SET ts = ts - 3600 WHERE message_id='%s';\""
        % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, rid4)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = send(r1, "msg.recall", {"messageId": rid4})
    check("recall expired 410", r["data"]["code"] == 410)

    # 10. 撤回图片后 media.url 403（对象存储门禁内）
    if medias_enabled:
        png_r = base64.b64decode(PNG_1X1_B64)
        r = send(r1, "media.presign", {"fileName": "rc.png", "mimeType": "image/png",
                                       "size": len(png_r), "kind": "image"})
        r_up = media_data(r).get("uploadUrl", "")
        r_fid = media_data(r).get("fileId", "")
        check("recall image presign ok", bool(r_up) and put(r_up, png_r) in (200, 204))
        r = send(r1, "media.commit", {"fileId": r_fid, "size": len(png_r),
                                      "width": 1, "height": 1})
        check("recall image commit ok", r["data"]["code"] == 0)
        r = send_msg(r1, RB, "", "rc-img-" + base, msg_type="image", media_id=r_fid)
        check("recall image send ok", r["data"]["code"] == 0)
        r = json.loads(rb.recv())
        rid_img = r["data"]["messageId"]
        json.loads(r1x.recv())
        r = send(r1, "msg.recall", {"messageId": rid_img})
        check("recall image ok", r["data"]["code"] == 0)
        json.loads(rb.recv()); json.loads(r1x.recv())  # notify
        r = send(rb, "media.url", {"fileId": r_fid})
        ent = media_data(r).get("urls", {}).get(r_fid, {})
        check("recall image url 403", ent.get("code") == 403)

    # 撤回群清理（群表按 owner 删除走统一清理段，此处仅关连接）
    r1.close(); r1x.close(); rb.close(); r3.close()

    # ================= 会话列表（conversation.list）=================
    # 独立账号；秒级 ts 用 sleep(1) 控制排序确定性。
    # 群 unread/lastReadTs 为 v3.2 占位（恒 0）——投递水位线≠已读游标，
    # 群已读能力未实现，断言语义而非真实值。
    V1, V2, V3 = base + "90", base + "91", base + "92"
    v1 = WS(); v2 = WS(); v3 = WS()
    for w, acct in ((v1, V1), (v2, V2), (v3, V3)):
        assert register(w, acct, "pw1234")["data"]["code"] == 0
        assert login(w, acct, "pw1234")["data"]["code"] == 0
    # 联系人 v1↔v2、v1↔v3（各 4 帧消耗：res/push/added/res）
    v1.send(env("contact.request", {"account": V2})); json.loads(v1.recv())
    v2.send(env("contact.accept", {"account": V1})); json.loads(v2.recv())
    json.loads(v1.recv()); json.loads(v2.recv())
    v1.send(env("contact.request", {"account": V3})); json.loads(v1.recv())
    v3.send(env("contact.accept", {"account": V1})); json.loads(v3.recv())
    json.loads(v1.recv()); json.loads(v3.recv())
    # 自己对话框（to=自己账号）：进列表必须被排除（resolveTarget 无 0=自己路径）
    r = send_msg(v1, V1, "自言自语", "cv-self-" + base)
    check("conv self send ok", r["data"]["code"] == 0)
    time.sleep(1.1)
    # t1: V2→v1 两条 → v1 视角 unread(V2)=2
    for i in range(2):
        r = send_msg(v2, V1, "v2-%d" % i, "cv2-%d-%s" % (i, base))
        check("conv v2 send %d" % i, r["data"]["code"] == 0)
        check("conv v1 recv %d" % i, json.loads(v1.recv())["type"] == "msg.recv")
    time.sleep(1.1)
    # t2: V3→v1 一条 → unread(V3)=1
    r = send_msg(v3, V1, "v3-0", "cv3-%s" % base)
    check("conv v3 send ok", r["data"]["code"] == 0)
    check("conv v1 recv v3", json.loads(v1.recv())["type"] == "msg.recv")
    time.sleep(1.1)
    # t3: v1→V2 一条 → v1 视角 V2 lastTs 更新但 unread 不变（自发不计未读）
    r = send_msg(v1, V2, "v1-reply", "cv1r-%s" % base)
    check("conv v1 reply ok", r["data"]["code"] == 0)
    check("conv v2 recv reply", json.loads(v2.recv())["type"] == "msg.recv")
    time.sleep(1.1)
    # t4/t5: 建群拉 V2，V2 群发一条（群会话最新）
    r = send(v1, "group.create", {"name": "会话群"})
    check("conv group create ok", r["data"]["code"] == 0)
    cgid = r["data"]["data"]["groupId"]
    r = send(v1, "group.invite", {"groupId": cgid, "account": V2})
    check("conv group invite ok", r["data"]["code"] == 0)
    json.loads(v1.recv())          # member_changed 推送
    json.loads(v2.recv())          # V2: group.added
    time.sleep(1.1)
    r = send_msg(v2, cgid, "群消息", "cg-%s" % base, chat_type="group")
    check("conv group send ok", r["data"]["code"] == 0)
    gmsg = json.loads(v1.recv())
    check("conv group recv", gmsg["type"] == "msg.recv" and gmsg["data"]["groupId"] == cgid)
    time.sleep(1.1)
    # t6: V2 撤回群消息 → 群条目 lastMsgRecalled=true（撤回行仍在末条位）
    r = send(v2, "msg.recall", {"messageId": gmsg["data"]["messageId"]})
    check("conv group recall ok", r["data"]["code"] == 0)
    check("conv group recall notify", json.loads(v1.recv())["type"] == "msg.recall_notify")
    time.sleep(1.1)
    # 列表断言（v1 视角）：群(t5) > V2(t3) > V3(t2)；自己对话框排除
    r = send(v1, "conversation.list", {"limit": 10})
    check("conv list res ok", r["type"] == "conversation.list_res"
          and r["data"]["code"] == 0)
    convs = r["data"]["data"]["conversations"]
    check("conv list self excluded", all(c.get("account") != V1 for c in convs)
          and len(convs) == 3)
    order = [c["lastMsgTs"] for c in convs]
    check("conv list sorted desc", order == sorted(order, reverse=True))
    check("conv list newest is group", convs[0]["chatType"] == "group"
          and convs[0]["groupId"] == cgid and convs[0]["groupName"] == "会话群")
    check("conv group unread placeholder 0", convs[0]["unreadCount"] == 0
          and convs[0]["lastReadTs"] == 0)
    check("conv group lastMsgRecalled", convs[0]["lastMsgRecalled"] is True
          and convs[0]["lastMsgType"] == "text")
    by_acct = {c["account"]: c for c in convs if c["chatType"] == "single"}
    check("conv v2 unread", by_acct[V2]["unreadCount"] == 2)
    check("conv v2 lastTs is my reply", by_acct[V2]["lastMsgTs"] > by_acct[V3]["lastMsgTs"])
    check("conv v3 unread", by_acct[V3]["unreadCount"] == 1)
    check("conv no leak", set(by_acct) <= {V2, V3})
    # 已读联动：msg.read → unread 归零 + lastReadTs 真实值
    send(v1, "msg.read", {"from": V2})
    r = send(v1, "conversation.list", {"limit": 10})
    convs = r["data"]["data"]["conversations"]
    by_acct = {c["account"]: c for c in convs if c["chatType"] == "single"}
    check("conv read zeroes unread", by_acct[V2]["unreadCount"] == 0
          and by_acct[V2]["lastReadTs"] > 0)
    # limit=2：只留最新两条（群 + V2），V3 被截
    r = send(v1, "conversation.list", {"limit": 2})
    convs2 = r["data"]["data"]["conversations"]
    check("conv limit 2", len(convs2) == 2 and convs2[0]["chatType"] == "group"
          and convs2[1].get("account") == V2)
    # 退群 → 群条目消失（群主不能退群：先转让给 V2 再退）
    r = send(v1, "group.transfer", {"groupId": cgid, "account": V2})
    check("conv transfer ok", r["data"]["code"] == 0)
    json.loads(v1.recv())  # owner_changed
    json.loads(v2.recv())  # owner_changed
    r = send(v1, "group.leave", {"groupId": cgid})
    check("conv group leave ok", r["data"]["code"] == 0)
    json.loads(v2.recv())  # V2: member_changed(leave)
    r = send(v1, "conversation.list", {"limit": 10})
    convs = r["data"]["data"]["conversations"]
    check("conv left group excluded",
          all(c["chatType"] != "group" for c in convs) and len(convs) == 2)
    # 对端视角（V2）：unread(v1)=1（v1-reply）；V2 已是群主，列表含群但单聊仅 V1
    r = send(v2, "conversation.list", {"limit": 10})
    v2convs = r["data"]["data"]["conversations"]
    v2by = {c["account"]: c for c in v2convs if c["chatType"] == "single"}
    check("conv peer view", len(v2by) == 1 and v2by[V1]["unreadCount"] == 1)
    v1.close(); v2.close(); v3.close()


    # ================= 可靠投递（delivery ack，权威 v3.3）=================
    # 仅在确认制(on)下运行：本段验证“未 ACK 即重登补发/ACK 后不补/连续前缀游标”。
    # off 模式保持旧语义（在线即 delivered、推送/补发推进水位线），不适用本段。
    ack_required = os.environ.get("IM_DELIVERY_ACK_REQUIRED") == "1"
    D1, D2, D3 = base + "94", base + "95", base + "96"
    if not ack_required:
        skip("delivery ack (on-mode)", "IM_DELIVERY_ACK_REQUIRED!=1（阶段A 旧语义，本段跳过）")
    else:
        # 单会话纪律：同一用户任一时刻只保留一条连接。
        d1 = WS(); d2 = WS(); d3 = WS()
        for w, acct in ((d1, D1), (d2, D2), (d3, D3)):
            assert register(w, acct, "pw1234")["data"]["code"] == 0
            assert login(w, acct, "pw1234")["data"]["code"] == 0
        d1.send(env("contact.request", {"account": D2})); json.loads(d1.recv())
        d2.send(env("contact.accept", {"account": D1})); json.loads(d2.recv())
        json.loads(d1.recv()); json.loads(d2.recv())
        time.sleep(1.1)

        # 1. msg.ack 回显 messageId（发送方即时拿到，可立即撤回）
        r = send_msg(d1, D2, "ack带ID", "ds0-" + base)
        check("dlv msg.ack has messageId", r["data"]["code"] == 0
              and bool(r["data"].get("messageId")))
        ackmid = r["data"]["messageId"]
        json.loads(d2.recv())  # D2 在线收 msg.recv

        # 2. 单聊在线未 ACK → 重登补发；ACK 后不再补；ACK 幂等
        mid1 = ackmid
        d2.close()
        d2b = WS(); assert login(d2b, D2, "pw1234")["data"]["code"] == 0
        off = json.loads(d2b.recv())
        check("dlv single unacked re-delivered", off["type"] == "msg.offline"
              and any(m["messageId"] == mid1 for m in off["data"]["msgs"]))
        r = send(d2b, "msg.offline_ack", {"messageIds": [mid1]})
        check("dlv single ack ok", r["data"]["code"] == 0)
        r = send(d2b, "msg.offline_ack", {"messageIds": [mid1]})
        check("dlv single ack idempotent", r["data"]["code"] == 0)
        d2b.close()
        d2c = WS(); assert login(d2c, D2, "pw1234")["data"]["code"] == 0
        got = None
        try:
            d2c.s.settimeout(1.5); got = json.loads(d2c.recv())
        except Exception:
            got = None
        d2c.s.settimeout(5)
        miss = got is None or got.get("type") != "msg.offline" \
            or all(m["messageId"] != mid1 for m in got["data"]["msgs"])
        check("dlv single acked not re-delivered", miss)
        d2 = d2c

        # 3. 撤回的 pending 单聊消息不再补发
        r = send_msg(d1, D2, "待撤回", "dr1-" + base)
        r = json.loads(d2.recv())
        rmid = r["data"]["messageId"]
        r = send(d1, "msg.recall", {"messageId": rmid})
        check("dlv recall pending ok", r["data"]["code"] == 0)
        json.loads(d2.recv())  # recall_notify
        d2.close()
        d2d = WS(); assert login(d2d, D2, "pw1234")["data"]["code"] == 0
        got = None
        try:
            d2d.s.settimeout(1.5); got = json.loads(d2d.recv())
        except Exception:
            got = None
        d2d.s.settimeout(5)
        found = got is not None and got.get("type") == "msg.offline" \
            and any(m["messageId"] == rmid for m in got["data"]["msgs"])
        check("dlv recalled pending not re-delivered", not found)
        d2 = d2d

        # 4. 群在线未 ACK → 重登补发；group.offline_ack{groupId,messageIds[]} 后不补
        r = send(d1, "group.create", {"name": "投递群"})
        check("dlv group create ok", r["data"]["code"] == 0)
        dgid = r["data"]["data"]["groupId"]
        r = send(d1, "group.invite", {"groupId": dgid, "account": D2})
        check("dlv group invite ok", r["data"]["code"] == 0)
        json.loads(d1.recv()); json.loads(d2.recv())
        time.sleep(1.1)
        r = send_msg(d1, dgid, "群在线未确认", "dg1-" + base, chat_type="group")
        check("dlv group ack has messageId", r["data"]["code"] == 0
              and bool(r["data"].get("messageId")))
        g1 = json.loads(d2.recv())
        check("dlv group online recv", g1["type"] == "msg.recv" and g1["data"]["groupId"] == dgid)
        d2.close()
        d2e = WS(); assert login(d2e, D2, "pw1234")["data"]["code"] == 0
        off = json.loads(d2e.recv())
        check("dlv group unacked re-delivered", off["type"] == "msg.offline"
              and any(m.get("messageId") == g1["data"]["messageId"] for m in off["data"]["msgs"]))
        r = send(d2e, "group.offline_ack", {"groupId": dgid,
                                            "messageIds": [g1["data"]["messageId"]]})
        check("dlv group ack ok", r["data"]["code"] == 0)
        r = send(d2e, "group.offline_ack", {"groupId": dgid,
                                            "messageIds": [g1["data"]["messageId"]]})
        check("dlv group ack idempotent", r["data"]["code"] == 0)
        d2e.close()
        d2f = WS(); assert login(d2f, D2, "pw1234")["data"]["code"] == 0
        got = None
        try:
            d2f.s.settimeout(1.5); got = json.loads(d2f.recv())
        except Exception:
            got = None
        d2f.s.settimeout(5)
        miss = got is None or got.get("type") != "msg.offline" \
            or all(m.get("messageId") != g1["data"]["messageId"] for m in got["data"]["msgs"])
        check("dlv group acked not re-delivered", miss)
        d2 = d2f
        # 5. 群 ACK 越权/非法
        r = send(d3, "group.offline_ack", {"groupId": dgid, "messageIds": [g1["data"]["messageId"]]})
        check("dlv group ack non-member 403", r["data"]["code"] == 403)
        r = send(d2, "group.offline_ack", {"groupId": dgid})
        check("dlv group ack bad params 400", r["data"]["code"] == 400)

        # 6. 同秒多条 + 连续前缀：ACK 第 2 条后重登只补第 3 条（(ts,id) 严格序）
        same = []
        for i in range(3):
            r = send_msg(d1, dgid, "同秒%d" % i, "dgs-%d-%s" % (i, base), chat_type="group")
            check("dlv same-sec send %d" % i, r["data"]["code"] == 0)
            same.append(json.loads(d2.recv())["data"])
        r = send(d2, "group.offline_ack", {"groupId": dgid,
                                           "messageIds": [same[0]["messageId"],
                                                          same[1]["messageId"]]})
        check("dlv same-sec ack prefix ok", r["data"]["code"] == 0)
        d2.close()
        d2g = WS(); assert login(d2g, D2, "pw1234")["data"]["code"] == 0
        off = json.loads(d2g.recv())
        mids = [m.get("messageId") for m in off.get("data", {}).get("msgs", [])] \
            if off.get("type") == "msg.offline" else []
        check("dlv same-sec only last backfilled",
              same[2]["messageId"] in mids and same[0]["messageId"] not in mids
              and same[1]["messageId"] not in mids)
        # 消费补发的 same[2] 并 ACK，使游标推进到 same[2]（为下一用例清出连续前缀）
        r = send(d2g, "group.offline_ack", {"groupId": dgid,
                                            "messageIds": [same[2]["messageId"]]})
        check("dlv same-sec ack last ok", r["data"]["code"] == 0)
        d2 = d2g

        # 7. 连续前缀缺口：服务端不得按最大 (ts,id) 跃迁
        #    D2 游标已到 same[1]；D1 发 gap-a, gap-b, gap-c；客户端只 ACK gap-a 与 gap-c
        #    → 游标只能推进到 gap-a（gap-b 空洞），重登应补 gap-b、gap-c
        gaps = []
        for i in range(3):
            r = send_msg(d1, dgid, "gap%d" % i, "dgp-%d-%s" % (i, base), chat_type="group")
            check("dlv gap send %d" % i, r["data"]["code"] == 0)
            gaps.append(json.loads(d2.recv())["data"])
        r = send(d2, "group.offline_ack",
                 {"groupId": dgid, "messageIds": [gaps[0]["messageId"], gaps[2]["messageId"]]})
        check("dlv gap ack holes ok", r["data"]["code"] == 0)
        d2.close()
        d2h = WS(); assert login(d2h, D2, "pw1234")["data"]["code"] == 0
        off = json.loads(d2h.recv())
        mids = [m.get("messageId") for m in off.get("data", {}).get("msgs", [])] \
            if off.get("type") == "msg.offline" else []
        check("dlv contiguous prefix not max-jump",
              gaps[1]["messageId"] in mids and gaps[2]["messageId"] in mids
              and gaps[0]["messageId"] not in mids)
        d2 = d2h

        # 8. 自身消息被游标跨过：D1 发 own-X 后离线，D2 发 peer-Y；D1 重登仅补 peer-Y
        r = send_msg(d1, dgid, "own-X", "dox-" + base, chat_type="group")
        check("dlv own X send ok", r["data"]["code"] == 0)
        gx = json.loads(d2.recv())
        check("dlv D2 recv own X", gx["type"] == "msg.recv")
        d1.close()
        time.sleep(1.1)
        r = send_msg(d2, dgid, "peer-Y", "doy-" + base, chat_type="group")
        check("dlv D2 send Y ok", r["data"]["code"] == 0)
        d1b = WS(); assert login(d1b, D1, "pw1234")["data"]["code"] == 0
        off = json.loads(d1b.recv())
        omsgs = off.get("data", {}).get("msgs", []) if off.get("type") == "msg.offline" else []
        mids8 = [m.get("messageId") for m in omsgs]
        gy = [m for m in omsgs if m.get("clientMsgId") == "doy-" + base]
        check("dlv own message skipped in backfill", gx["data"]["messageId"] not in mids8)
        check("dlv peer message backfilled", len(gy) == 1)
        r = send(d1b, "group.offline_ack",
                 {"groupId": dgid, "messageIds": [m["messageId"] for m in omsgs]})
        check("dlv ack crosses own message", r["data"]["code"] == 0)
        d1b.close()
        d2.close(); d3.close()

        # ===== T1–T7 群投递游标 membership epoch（P0-1/P0-2 修复回归）=====
        # 权威 spec：docs/superpowers/specs/2026-10-03-group-cursor-membership-epoch-design.md
        def drain_ws(w, tmo=0.6):
            # 消费并返回当前已到达的全部帧（登录补发/事件推送清场），超时结束
            w.s.settimeout(tmo)
            frames = []
            try:
                while True:
                    frames.append(json.loads(w.recv()))
            except Exception:
                pass
            w.s.settimeout(5)
            return frames

        def offline_ids(frames):
            return [m.get("messageId") for f in frames if f.get("type") == "msg.offline"
                    for m in f.get("data", {}).get("msgs", [])]

        t1a = WS(); t1b = WS()
        assert login(t1a, D1, "pw1234")["data"]["code"] == 0
        assert login(t1b, D2, "pw1234")["data"]["code"] == 0
        # 清场：历史段遗留的未确认补发（旧群 dgid 等）不影响本组断言
        drain_ws(t1a); drain_ws(t1b)

        # T1: 新成员加入已有历史 → 入群前消息不进 ACK 窗口；M1 可 ACK；重连不重复
        r = send(t1a, "group.create", {"name": "纪元T1"})
        check("epoch T1 group create ok", r["data"]["code"] == 0)
        eg1 = r["data"]["data"]["groupId"]
        pres = []
        for i in range(3):
            r = send_msg(t1a, eg1, "T1PRE%d" % i, "t1pre%d-%s" % (i, base), chat_type="group")
            pres.append(r["data"]["messageId"])
        time.sleep(1.1)  # 保证 M1/入群边界与 PRE 不同秒
        r = send(t1a, "group.invite", {"groupId": eg1, "account": D2})
        check("epoch T1 invite ok", r["data"]["code"] == 0)
        drain_ws(t1a)  # member_changed
        json.loads(t1b.recv())  # group.added
        time.sleep(1.1)
        r = send_msg(t1a, eg1, "T1M1", "t1m1-%s" % base, chat_type="group")
        m1 = r["data"]["messageId"]
        gg = json.loads(t1b.recv())
        check("epoch T1 online recv M1", gg.get("data", {}).get("messageId") == m1)
        r = send(t1b, "group.offline_ack", {"groupId": eg1, "messageIds": [m1]})
        check("epoch T1 ack M1 ok", r["data"]["code"] == 0)
        t1b.close(); time.sleep(0.4)
        t1b2 = WS(); assert login(t1b2, D2, "pw1234")["data"]["code"] == 0
        omids = offline_ids(drain_ws(t1b2, 1.2))
        check("epoch T1 pre-history never delivered", all(p not in omids for p in pres))
        check("epoch T1 acked M1 not re-delivered", m1 not in omids)

        # T2/T3: 退群 → 群内消息 → 重入 → 新纪元在线 Y1/Y2 ACK → 重连不重复
        r = send(t1b2, "group.leave", {"groupId": eg1})
        check("epoch T2 leave ok", r["data"]["code"] == 0)
        drain_ws(t1a)
        time.sleep(1.1)
        x1 = send_msg(t1a, eg1, "T2X1", "t2x1-%s" % base, chat_type="group")["data"]["messageId"]
        time.sleep(1.1)
        x2 = send_msg(t1a, eg1, "T2X2", "t2x2-%s" % base, chat_type="group")["data"]["messageId"]
        r = send(t1a, "group.invite", {"groupId": eg1, "account": D2})
        check("epoch T2 re-invite ok", r["data"]["code"] == 0)
        drain_ws(t1a)              # member_changed
        json.loads(t1b2.recv())    # group.added（在线）
        time.sleep(1.1)
        y1 = send_msg(t1a, eg1, "T3Y1", "t3y1-%s" % base, chat_type="group")["data"]["messageId"]
        gg = json.loads(t1b2.recv())
        check("epoch T3 online recv Y1", gg.get("data", {}).get("messageId") == y1)
        y2 = send_msg(t1a, eg1, "T3Y2", "t3y2-%s" % base, chat_type="group")["data"]["messageId"]
        gg = json.loads(t1b2.recv())
        check("epoch T3 online recv Y2", gg.get("data", {}).get("messageId") == y2)
        r = send(t1b2, "group.offline_ack", {"groupId": eg1, "messageIds": [y1, y2]})
        check("epoch T3 ack Y1Y2 ok", r["data"]["code"] == 0)
        t1b2.close(); time.sleep(0.4)
        t1b3 = WS(); assert login(t1b3, D2, "pw1234")["data"]["code"] == 0
        omids = offline_ids(drain_ws(t1b3, 1.2))
        check("epoch T2 old epoch cursor not inherited (X not replayed)",
              x1 not in omids and x2 not in omids)
        check("epoch T3 acked Y1Y2 not re-delivered", y1 not in omids and y2 not in omids)

        # T4: 入群边界同秒——SPRE 与 SPOST 尽量同秒，按 (ts,id) 精确归属
        r = send(t1b3, "group.leave", {"groupId": eg1})
        check("epoch T4 leave ok", r["data"]["code"] == 0)
        drain_ws(t1a)
        while (time.time() % 1) > 0.15:  # 卡秒初，压缩 SPRE→invite→SPOST 时差
            time.sleep(0.01)
        spre = send_msg(t1a, eg1, "T4PRE", "t4pre-%s" % base, chat_type="group")["data"]["messageId"]
        r = send(t1a, "group.invite", {"groupId": eg1, "account": D2})
        check("epoch T4 invite ok", r["data"]["code"] == 0)
        drain_ws(t1a, 0.3)  # 消费 member_changed（拉起者自身端也收），防误读为 msg.ack
        t1b3.close()        # 单会话纪律：D2 旧连接下线后再新登录
        spost = send_msg(t1a, eg1, "T4POST", "t4post-%s" % base, chat_type="group")["data"]["messageId"]
        drain_ws(t1a)
        t1b4 = WS(); assert login(t1b4, D2, "pw1234")["data"]["code"] == 0
        omids = offline_ids(drain_ws(t1b4, 1.2))
        check("epoch T4 post-join message deliverable", spost in omids)
        check("epoch T4 pre-join message not delivered", spre not in omids)
        r = send(t1b4, "group.offline_ack", {"groupId": eg1, "messageIds": [spost]})
        check("epoch T4 ack post ok", r["data"]["code"] == 0)
        t1b4.close(); time.sleep(0.4)
        t1b5 = WS(); assert login(t1b5, D2, "pw1234")["data"]["code"] == 0
        omids = offline_ids(drain_ws(t1b5, 1.2))
        check("epoch T4 cursor advanced (no re-delivery)",
              spost not in omids and spre not in omids)

        # T6: recalled 位于 ACK 前缀中间——无需 ACK 即可被游标跨过
        a1 = send_msg(t1a, eg1, "T6A1", "t6a1-%s" % base, chat_type="group")["data"]["messageId"]
        gg = json.loads(t1b5.recv())
        check("epoch T6 online recv A1", gg.get("data", {}).get("messageId") == a1)
        a2 = send_msg(t1a, eg1, "T6A2", "t6a2-%s" % base, chat_type="group")["data"]["messageId"]
        gg = json.loads(t1b5.recv())
        check("epoch T6 online recv A2", gg.get("data", {}).get("messageId") == a2)
        a3 = send_msg(t1a, eg1, "T6A3", "t6a3-%s" % base, chat_type="group")["data"]["messageId"]
        gg = json.loads(t1b5.recv())
        check("epoch T6 online recv A3", gg.get("data", {}).get("messageId") == a3)
        r = send(t1a, "msg.recall", {"messageId": a2})
        check("epoch T6 recall A2 ok", r["data"]["code"] == 0)
        json.loads(t1b5.recv())  # recall_notify
        r = send(t1b5, "group.offline_ack", {"groupId": eg1, "messageIds": [a1, a3]})
        check("epoch T6 ack A1A3 (A2 recalled) ok", r["data"]["code"] == 0)
        t1b5.close(); time.sleep(0.4)
        t1b6 = WS(); assert login(t1b6, D2, "pw1234")["data"]["code"] == 0
        omids = offline_ids(drain_ws(t1b6, 1.2))
        check("epoch T6 recalled crossed (A3 not re-delivered)",
              a1 not in omids and a3 not in omids)

        # T7: ACK 落库后响应丢失即断线（不重放）；未 ACK 断线（必须重放）
        b1 = send_msg(t1a, eg1, "T7B1", "t7b1-%s" % base, chat_type="group")["data"]["messageId"]
        gg = json.loads(t1b6.recv())
        check("epoch T7 online recv B1", gg.get("data", {}).get("messageId") == b1)
        t1b6.send(env("group.offline_ack", {"groupId": eg1, "messageIds": [b1]}))
        time.sleep(0.3)  # 让 ACK 先到服务端，再断线（不读 ack_res）
        t1b6.close(); time.sleep(0.4)
        t1b7 = WS(); assert login(t1b7, D2, "pw1234")["data"]["code"] == 0
        omids = offline_ids(drain_ws(t1b7, 1.2))
        check("epoch T7 raced ack persisted (B1 not replayed)", b1 not in omids)
        b2 = send_msg(t1a, eg1, "T7B2", "t7b2-%s" % base, chat_type="group")["data"]["messageId"]
        gg = json.loads(t1b7.recv())
        check("epoch T7 online recv B2", gg.get("data", {}).get("messageId") == b2)
        t1b7.close(); time.sleep(0.4)  # 未 ACK 即断线
        t1b8 = WS(); assert login(t1b8, D2, "pw1234")["data"]["code"] == 0
        omids = offline_ids(drain_ws(t1b8, 1.2))
        check("epoch T7 unacked B2 replayed", b2 in omids)
        r = send(t1b8, "group.offline_ack", {"groupId": eg1, "messageIds": [b2]})
        check("epoch T7 ack B2 ok", r["data"]["code"] == 0)
        t1b8.close(); t1a.close()

    a.close(); b.close()

    # 清理本次注册的测试账号及其关联消息（精确账号列表，不误删真实用户）
    import subprocess
    used = [A, B, C, SINK, SPAM, SINK2, SPAM2, D, E, F, G, H, base + "50", G1, G2,
            M1, M2, M3, M4, PIMG, QIMG, CIMG, RA, RB, RC, V1, V2, V3, D1, D2, D3] \
        + cap_accounts + [OVER, X, Y, Z, BL, BL2, BL3, BL4]
    inlist = ",".join("'%s'" % x for x in used)
    # 群表清理必须在 users 删除之前（按 account 查 user_id，无级联外键）
    subprocess.run(["bash", "-c",
        "MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s -e \""
        "USE IMS_reactor_db; "
        "DELETE FROM group_members WHERE user_id IN (SELECT id FROM users WHERE account IN (%s)); "
        "DELETE FROM \`groups\` WHERE owner_id IN (SELECT id FROM users WHERE account IN (%s));\" 2>/dev/null"
        % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, inlist, inlist)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 媒体表清理：按本次测试账号 uploader_id 精确删除（先于 users 删除，账号仍在）
    subprocess.run(["bash", "-c",
        "MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s -e \""
        "USE IMS_reactor_db; "
        "DELETE FROM media WHERE uploader_id IN (SELECT id FROM users WHERE account IN (%s));\" 2>/dev/null"
        % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, inlist)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["bash", "-c",
        "MYSQL_PWD=%s mysql -u IMS_jack -h 127.0.0.1 -P %s -e \""
        "USE IMS_reactor_db; "
        "DELETE m FROM messages m JOIN users u ON m.from_user=u.id OR m.to_user=u.id "
        "WHERE u.account IN (%s); "
        "DELETE FROM blocks WHERE user_id IN (SELECT id FROM users WHERE account IN (%s)) "
        "OR blocked_id IN (SELECT id FROM users WHERE account IN (%s)); "
        "DELETE FROM users WHERE account IN (%s);\" 2>/dev/null"
        % (MYSQL_PWD_ENV, MYSQL_PORT_ENV, inlist, inlist, inlist, inlist)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    fails = [n for n, c in results if not c]
    suffix = ""
    if skipped:
        suffix = f", {len(skipped)} skipped (" + "; ".join(r for _, r in skipped) + ")"
    print(f"\n{len(results)} tests, {len(fails)} failures{suffix}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
