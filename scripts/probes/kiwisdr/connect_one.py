"""One short SND session against one KiwiSDR, the way our client connects (path /kiwi/<ts>/SND).
Usage: python connect_one.py <scheme http|https> <host> <port> [seconds]"""
import base64, http.client, json, os, socket, ssl, struct, sys, time

sys.stdout.reconfigure(encoding="utf-8")
scheme, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 6.0
tls = scheme == "https"
UA = "sdriak-test"


def ver():
    c = (http.client.HTTPSConnection(host, port, timeout=8, context=ssl.create_default_context()) if tls
         else http.client.HTTPConnection(host, port, timeout=8))
    c.request("GET", "/VER", headers={"User-Agent": UA})
    r = c.getresponse()
    return json.loads(r.read())["ts"]


def ws_open(path):
    s = socket.create_connection((host, port), 8)
    if tls:
        s = ssl.create_default_context().wrap_socket(s, server_hostname=host)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nUser-Agent: {UA}\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d:
            raise EOFError("closed in handshake")
        buf += d
    hdr, rest = buf.split(b"\r\n\r\n", 1)
    return s, hdr.split(b"\r\n")[0].decode(), rest


def send(s, txt):
    p = txt.encode()
    m = os.urandom(4)
    s.sendall(bytes([0x81, 0x80 | len(p)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(p)))


class Reader:
    def __init__(self, s, rest):
        self.s, self.buf = s, rest

    def n(self, k):
        while len(self.buf) < k:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("socket closed")
            self.buf += d
        r, self.buf = self.buf[:k], self.buf[k:]
        return r

    def frame(self):
        msg = b""
        while True:
            b0, b1 = self.n(2)
            k = b1 & 0x7F
            if k == 126:
                k = struct.unpack(">H", self.n(2))[0]
            elif k == 127:
                k = struct.unpack(">Q", self.n(8))[0]
            d = self.n(k)
            op = b0 & 0x0F
            if op == 8:
                raise EOFError("ws close")
            if op in (9, 10):
                continue
            msg += d
            if b0 & 0x80:
                return msg


res = {"target": f"{scheme}://{host}:{port}"}
allmsg = []
try:
    ts = ver()
    s, status, rest = ws_open(f"/kiwi/{ts}/SND")
    res["handshake"] = status
    if " 101 " in status:
        rd = Reader(s, rest)
        for c in ["SET auth t=kiwi p=#", "SET AR OK in=12000 out=48000", f"SERVER DE CLIENT {UA} SND",
                  f"SET ident_user={UA}", "SET compression=0",
                  "SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50",
                  "SET mod=iq low_cut=-5900 high_cut=5900 freq=7100.000"]:
            send(s, c)
        s.settimeout(15)
        t0 = time.monotonic()
        pk, flags, keys, sizes = 0, {}, {}, set()
        while time.monotonic() - t0 < secs:
            m = rd.frame()
            if m[:3] == b"SND":
                pk += 1
                flags[hex(m[3])] = flags.get(hex(m[3]), 0) + 1
                sizes.add(len(m))
            elif m[:3] == b"MSG":
                allmsg.append(m[4:200].decode(errors="replace"))
                for kv in m[4:].decode(errors="replace").split(" "):
                    k, _, v = kv.partition("=")
                    if k in ("too_busy", "badp", "redirect", "down", "audio_rate", "sample_rate", "monitor", "camp"):
                        keys[k] = v[:60]
        res.update({"snd_packets": pk, "flags": flags, "sizes": sorted(sizes), "msgs": keys,
                    "our_client_would_accept": sum(v for k, v in flags.items() if k == "0x8")})
        try:
            s.sendall(bytes([0x88, 0x82]) + b"\0\0\0\0" + b"\x03\xe8")
        except Exception:
            pass
    s.close()
except Exception as e:
    res["error"] = f"{type(e).__name__}: {e}"
    res["msgs_before_error"] = [x for x in allmsg if not x.startswith(("load_", "cfg_", "extint"))][:25]
print(json.dumps(res, ensure_ascii=False))
