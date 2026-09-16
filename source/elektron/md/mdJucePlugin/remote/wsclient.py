#!/usr/bin/env python3
"""Tiny WebSocket client for testing the remote panel server without a browser.

  wsclient.py <host:port> [messages...]     e.g. wsclient.py localhost:8790 "b Trigger1 1" "b Trigger1 0"

Prints a summary of every state frame received (lit pixel count, LED banks) for a few seconds.
"""
import base64, os, socket, struct, sys, time

def ws_connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (host, key)).encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(1024)
        if not chunk: raise RuntimeError("closed during handshake")
        resp += chunk
    status = resp.split(b"\r\n")[0]
    if b"101" not in status: raise RuntimeError("handshake failed: " + status.decode(errors="replace"))
    return s

def ws_send_text(s, text):
    data = text.encode(); mask = os.urandom(4)
    hdr = bytes([0x81])
    if len(data) < 126: hdr += bytes([0x80 | len(data)])
    else: hdr += bytes([0x80 | 126]) + struct.pack(">H", len(data))
    s.sendall(hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))

def ws_recv(s):
    def readn(n):
        buf = b""
        while len(buf) < n:
            c = s.recv(n - len(buf))
            if not c: raise RuntimeError("closed")
            buf += c
        return buf
    h = readn(2); op = h[0] & 0x0f; ln = h[1] & 0x7f
    if ln == 126: ln = struct.unpack(">H", readn(2))[0]
    elif ln == 127: ln = struct.unpack(">Q", readn(8))[0]
    return op, readn(ln)

def summarize(payload):
    if len(payload) < 2 + 1024 + 14 or payload[0] != 0x53: return "?"
    vram = payload[2:2 + 1024]; leds = payload[2 + 1024:2 + 1024 + 14]
    lit = sum(bin(b).count("1") for b in vram)
    return "model=%d lcd lit px=%d leds=%s" % (payload[1], lit, " ".join("%02x" % b for b in leds))

if __name__ == "__main__":
    host, port = sys.argv[1].split(":")
    s = ws_connect(host, int(port))
    s.settimeout(0.5)
    print("connected")
    ws_send_text(s, "hello")
    t0 = time.time(); msgs = sys.argv[2:]; nexti = 0; lastsend = t0
    frames = 0
    while time.time() - t0 < 4.0:
        if nexti < len(msgs) and time.time() - lastsend > 0.3:
            print("send:", msgs[nexti]); ws_send_text(s, msgs[nexti]); nexti += 1; lastsend = time.time()
        try:
            op, payload = ws_recv(s)
        except socket.timeout:
            continue
        if op == 2:
            frames += 1
            if frames <= 3 or frames % 20 == 0: print("frame %d: %s" % (frames, summarize(payload)))
        elif op == 8:
            print("server closed"); break
    print("%d state frames in 4 s" % frames)
