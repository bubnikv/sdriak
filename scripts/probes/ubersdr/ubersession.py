"""One gentle UberSDR native-API session: /api/description, /connection, /ws (iq48), /ws/user-spectrum (v2 binary8).
Records raw frames with arrival times; analysis is done offline by uberanalyze.py.
Usage: python ubersession.py <host> <port> <tls 0|1> <carrier_hz> <outfile.pkl>"""
import base64, json, os, pickle, socket, ssl, struct, sys, threading, time, uuid, http.client

sys.stdout.reconfigure(encoding="utf-8")
HOST, PORT, TLS, CARRIER, OUTF = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "1", float(sys.argv[4]), sys.argv[5]
UA = "sdriak-test"
T0 = time.monotonic()
now = lambda: time.monotonic() - T0
rec = {"host": HOST, "port": PORT, "tls": TLS, "carrier_hint": CARRIER, "events": []}


def ev(kind, **kw):
    kw.update(kind=kind, t=round(now(), 4))
    rec["events"].append(kw)


def http_req(method, path, body=None):
    c = (http.client.HTTPSConnection(HOST, PORT, timeout=15, context=ssl.create_default_context()) if TLS
         else http.client.HTTPConnection(HOST, PORT, timeout=15))
    hdr = {"User-Agent": UA}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        hdr["Content-Type"] = "application/json"
    c.request(method, path, body=data, headers=hdr)
    r = c.getresponse()
    return r.status, r.read(400000).decode("utf-8", "replace")


class WS:
    def __init__(self, name, path):
        self.name = name
        raw = socket.create_connection((HOST, PORT), 15)
        if TLS:
            raw = ssl.create_default_context().wrap_socket(raw, server_hostname=HOST)
        self.sock = raw
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nUser-Agent: {UA}\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = self.sock.recv(4096)
            if not d:
                raise EOFError("closed in handshake")
            buf += d
        hdr, self.buf = buf.split(b"\r\n\r\n", 1)
        self.status = hdr.split(b"\r\n")[0].decode(errors="replace")
        self.body_on_fail = self.buf[:300].decode(errors="replace")
        self.lock = threading.Lock()
        self.frames = []          # (t, op, bytes)
        self.closed = None
        if " 101 " in self.status:
            self.th = threading.Thread(target=self._rx, daemon=True)
            self.th.start()

    def ok(self):
        return " 101 " in self.status

    def _n(self, k):
        while len(self.buf) < k:
            d = self.sock.recv(65536)
            if not d:
                raise EOFError("socket closed")
            self.buf += d
        r, self.buf = self.buf[:k], self.buf[k:]
        return r

    def _rx(self):
        try:
            while True:
                msg, op0 = b"", None
                while True:
                    b0, b1 = self._n(2)
                    k = b1 & 0x7F
                    if k == 126:
                        k = struct.unpack(">H", self._n(2))[0]
                    elif k == 127:
                        k = struct.unpack(">Q", self._n(8))[0]
                    d = self._n(k)
                    op = b0 & 0x0F
                    if op == 8:
                        raise EOFError("ws close " + d[:2].hex() + " " + d[2:].decode(errors="replace"))
                    if op == 9:
                        self.send(d, 10)
                        continue
                    if op == 10:
                        continue
                    if op:
                        op0 = op
                    msg += d
                    if b0 & 0x80:
                        break
                self.frames.append((round(now(), 5), op0, msg))
        except Exception as e:
            self.closed = (round(now(), 3), str(e))

    def send(self, payload, op=1):
        if isinstance(payload, (dict, list)):
            payload = json.dumps(payload)
        if isinstance(payload, str):
            payload = payload.encode()
        m = os.urandom(4)
        n = len(payload)
        h = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        with self.lock:
            self.sock.sendall(h + m + bytes(b ^ m[i % 4] for i, b in enumerate(payload)))
        if op == 1:
            ev("send", ws=self.name, msg=payload.decode(errors="replace")[:200])

    def close(self):
        try:
            self.send(b"\x03\xe8", 8)
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


def texts(ws, since=0.0):
    out = []
    for t, op, m in ws.frames:
        if op == 1 and t >= since:
            try:
                out.append((t, json.loads(m)))
            except Exception:
                pass
    return out


def wait_pong(ws, t0, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for t, j in texts(ws, t0):
            if j.get("type") == "pong":
                return round((t - t0) * 1e3, 1)
        time.sleep(0.01)
    return None


def main():
    sid = str(uuid.uuid4())
    rec["user_session_id"] = sid
    try:
        st, body = http_req("GET", "/api/description")
        rec["description_status"] = st
        rec["description"] = json.loads(body) if st == 200 else body[:300]
    except Exception as e:
        rec["description_error"] = str(e)
    time.sleep(1.0)
    st, body = http_req("POST", "/connection", {"user_session_id": sid})
    rec["connection_status"] = st
    try:
        rec["connection"] = json.loads(body)
    except Exception:
        rec["connection"] = body[:300]
    print("connection:", st, json.dumps(rec["connection"], ensure_ascii=False)[:400], flush=True)
    if not (isinstance(rec["connection"], dict) and rec["connection"].get("allowed")):
        return
    allowed = rec["connection"].get("allowed_iq_modes") or []
    if "iq48" not in allowed:
        rec["skip"] = "iq48 not allowed"
        return
    centre = CARRIER - 6000.0           # carrier sits at +6 kHz in the 48 kHz window
    iq = WS("iq", f"/ws?frequency={int(centre)}&mode=iq48&user_session_id={sid}&format=pcm-zstd&version=4")
    rec["iq_handshake"] = iq.status
    if not iq.ok():
        rec["iq_fail_body"] = iq.body_on_fail
        return
    time.sleep(0.3)
    iq.send({"type": "get_status"})
    time.sleep(1.5)
    sp = WS("spec", f"/ws/user-spectrum?user_session_id={sid}&mode=binary8&version=2")
    rec["spec_handshake"] = sp.status
    if not sp.ok():
        rec["spec_fail_body"] = sp.body_on_fail
    time.sleep(8.0)                      # baseline: IQ + default shared spectrum view
    # RTT, app-level ping on both sockets
    rtt = {"iq": [], "spec": []}
    for i in range(3):
        t0 = now(); iq.send({"type": "ping"}); rtt["iq"].append(wait_pong(iq, t0))
        if sp.ok():
            t0 = now(); sp.send({"type": "ping"}); rtt["spec"].append(wait_pong(sp, t0))
        time.sleep(0.7)
    rec["rtt_ms"] = rtt
    # IQ retunes: +-2 kHz, every 3 s
    for i in range(4):
        f = centre + (2000.0 if i % 2 == 0 else 0.0)
        ev("tune", f=f)
        iq.send({"type": "tune", "frequency": int(f), "mode": "iq48"})
        time.sleep(3.0)
    if sp.ok():
        # spectrum view changes, at most ~1 command per 3 s, then one 5-command burst
        sp.send({"type": "get_status"}); time.sleep(1.5)
        for label, f, bw in (("zoom100", CARRIER, 100.0), ("pan+5k", CARRIER + 5000.0, 100.0),
                             ("zoom20", CARRIER, 20.0), ("zoom5", CARRIER, 5.0)):
            ev("view", label=label, f=f, bw=bw)
            sp.send({"type": "zoom", "frequency": int(f), "binBandwidth": bw})
            time.sleep(3.0)
        ev("burst_start")
        for i in range(5):
            sp.send({"type": "pan", "frequency": int(CARRIER + 1000.0 * (i + 1)), "binBandwidth": 20.0})
            time.sleep(0.2)
        time.sleep(3.0)
        ev("reset")
        sp.send({"type": "reset"})
        time.sleep(3.0)
    rec["iq_closed"] = iq.closed
    rec["spec_closed"] = sp.closed if sp.ok() else None
    iq.close()
    if sp.ok():
        sp.close()
    rec["iq_frames"] = iq.frames
    rec["spec_frames"] = sp.frames if sp.ok() else []
    # U6: iq96 briefly, if allowed
    if "iq96" in allowed:
        time.sleep(4.0)
        st, body = http_req("POST", "/connection", {"user_session_id": sid})
        iq2 = WS("iq96", f"/ws?frequency={int(centre)}&mode=iq96&user_session_id={sid}&format=pcm-zstd&version=4")
        rec["iq96_handshake"] = iq2.status
        if iq2.ok():
            time.sleep(6.0)
            rec["iq96_frames"] = iq2.frames
            iq2.close()


try:
    main()
except Exception as e:
    rec["fatal"] = f"{type(e).__name__}: {e}"
    print("fatal:", rec["fatal"], flush=True)
rec["duration_s"] = round(now(), 2)
pickle.dump(rec, open(OUTF, "wb"))
print("saved", OUTF, "iq frames", len(rec.get("iq_frames", [])), "spec frames", len(rec.get("spec_frames", [])),
      "duration", rec["duration_s"], flush=True)
os._exit(0)
