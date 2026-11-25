"""Second UberSDR batch. Usage: python ubersession2.py <mode vfo|ret|lvl> <host> <port> <tls 0|1> <out.pkl>
vfo: two IQ VFOs + third-VFO probe + back-pressure (IQ, spectrum)
ret: 10 single retunes, a 15/s drag burst, far jumps, reconnect with the same user_session_id
lvl: levels (zoomed spectrum vs IQ vs header power) and spectrum edge cases + set_rate"""
import base64, json, os, pickle, socket, ssl, struct, sys, threading, time, uuid, http.client

sys.stdout.reconfigure(encoding="utf-8")
MODE, HOST, PORT, TLS, OUTF = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4] == "1", sys.argv[5]
UA = "sdriak-test"
T0 = time.monotonic()
now = lambda: time.monotonic() - T0
TDF = 162000.0
rec = {"mode": MODE, "host": HOST, "events": [], "sockets": {}}


def ev(kind, **kw):
    kw.update(kind=kind, t=round(now(), 4))
    rec["events"].append(kw)


def http_req(method, path, body=None):
    c = (http.client.HTTPSConnection(HOST, PORT, timeout=15, context=ssl.create_default_context()) if TLS
         else http.client.HTTPConnection(HOST, PORT, timeout=15))
    hdr = {"User-Agent": UA}
    data = json.dumps(body).encode() if body is not None else None
    if data:
        hdr["Content-Type"] = "application/json"
    c.request(method, path, body=data, headers=hdr)
    r = c.getresponse()
    return r.status, r.read(400000).decode("utf-8", "replace")


class WS:
    def __init__(self, name, path):
        self.name, self.paused, self.frames, self.closed = name, False, [], None
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
        self.fail_body = self.buf[:300].decode(errors="replace")
        self.lock = threading.Lock()
        rec["sockets"][name] = self
        ev("ws_open", ws=name, status=self.status, path=path.split("?")[0])
        if self.ok():
            threading.Thread(target=self._rx, daemon=True).start()

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
                while self.paused:
                    time.sleep(0.005)
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
                        self.send(d, 10); continue
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
            ev("ws_closed", ws=self.name, why=str(e)[:200])

    def send(self, payload, op=1):
        if isinstance(payload, (dict, list)):
            payload = json.dumps(payload)
        if isinstance(payload, str):
            payload = payload.encode()
        m = os.urandom(4)
        n = len(payload)
        h = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        try:
            with self.lock:
                self.sock.sendall(h + m + bytes(b ^ m[i % 4] for i, b in enumerate(payload)))
        except Exception as e:
            ev("send_fail", ws=self.name, err=str(e)); return
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


def iq_ws(name, sid, centre, mode="iq48"):
    return WS(name, f"/ws?frequency={int(centre)}&mode={mode}&user_session_id={sid}&format=pcm-zstd&version=4")


def spec_ws(sid):
    return WS("spec", f"/ws/user-spectrum?user_session_id={sid}&mode=binary8&version=2")


def admit(sid):
    st, body = http_req("POST", "/connection", {"user_session_id": sid})
    try:
        j = json.loads(body)
    except Exception:
        j = body[:300]
    ev("connection", status=st, body=j)
    if isinstance(j, dict):
        rec["allowed_iq_modes"] = j.get("allowed_iq_modes", [])
    print("connection:", st, json.dumps(j)[:300], flush=True)
    return isinstance(j, dict) and j.get("allowed")


def tune(ws, f, mode="iq48"):
    ev("tune", ws=ws.name, f=f)
    ws.send({"type": "tune", "frequency": int(f), "mode": mode})


def run_vfo(sid):
    a = iq_ws("iqA", sid, TDF - 6000)                 # TDF at +6 kHz
    time.sleep(1.0)
    b = iq_ws("iqB", sid, 77500 - 6000)               # DCF77 at +6 kHz
    time.sleep(1.0)
    sp = spec_ws(sid)
    time.sleep(6.0)
    tune(b, 77500 - 4000)                             # retune B only: whose socket echoes?
    time.sleep(3.0)
    c = iq_ws("iqC", sid, 225000 - 6000)              # third VFO probe
    time.sleep(3.0)
    rec["iqC_status"] = c.status
    rec["iqC_body"] = c.fail_body
    if c.ok():
        c.close()
    time.sleep(2.0)
    for pause in (2.0, 5.0):                          # back-pressure on A only
        ev("pause_start", ws="iqA", secs=pause)
        a.paused = True
        time.sleep(pause)
        a.paused = False
        ev("pause_end", ws="iqA")
        time.sleep(4.0)
    ev("pause_start", ws="spec", secs=5.0)            # back-pressure on spectrum
    sp.paused = True
    time.sleep(5.0)
    sp.paused = False
    ev("pause_end", ws="spec")
    time.sleep(5.0)
    for w in (a, b, sp):
        w.close()


def run_ret(sid):
    a = iq_ws("iqA", sid, TDF - 6000)
    time.sleep(4.0)
    base = TDF - 6000
    for i in range(10):                                # single retunes, 2.5 s apart
        tune(a, base + (2000.0 if i % 2 == 0 else 0.0))
        time.sleep(2.5)
    ev("burst_start")
    for i in range(15):                                # drag: 15 commands at 15/s, 100 Hz steps
        tune(a, base + 100.0 * (i + 1))
        time.sleep(1 / 15)
    ev("burst_end")
    time.sleep(3.0)
    for f in (7_000_000, 14_000_000, base):            # far jumps, then back
        tune(a, f)
        time.sleep(2.5)
    a.close()
    time.sleep(3.0)
    r = iq_ws("iqR", sid, base)                        # reconnect, same sid, no new /connection
    time.sleep(4.0)
    rec["reconnect_status"] = r.status
    rec["reconnect_body"] = r.fail_body
    if r.ok():
        r.close()


def run_lvl(sid):
    a = iq_ws("iqA", sid, TDF - 6000)
    time.sleep(1.0)
    sp = spec_ws(sid)
    time.sleep(4.0)
    a.send({"type": "get_status"})
    views = [("tdf_5Hz", TDF, 5.0), ("tdf_2Hz", TDF, 2.0), ("tdf_1Hz", TDF, 1.0), ("tdf_0.5Hz", TDF, 0.5),
             ("tdf_20Hz", TDF, 20.0), ("edge_low_100Hz", 1000.0, 100.0), ("edge_low_1kHz", 10000.0, 1000.0),
             ("edge_high_1kHz", 29_995_000.0, 1000.0), ("tdf_50Hz", TDF, 50.0)]
    for label, f, bw in views:
        ev("view", label=label, f=f, bw=bw)
        sp.send({"type": "zoom", "frequency": int(f), "binBandwidth": bw})
        time.sleep(3.5)
    for d in (4, 1):
        ev("set_rate", divisor=d)
        sp.send({"type": "set_rate", "divisor": d})
        time.sleep(5.0)
    for w in (a, sp):
        w.close()


def run_vfo2(sid):
    a = iq_ws("iqA", sid, TDF - 6000)                                     # primary, wide IQ
    time.sleep(3.0)
    b = WS("iqB", f"/ws?frequency=77500&mode=iq&user_session_id={sid}&format=pcm-zstd&version=4&vfo=1")
    rec["iqB_status"], rec["iqB_body"] = b.status, b.fail_body
    time.sleep(4.0)
    c = WS("iqC", f"/ws?frequency=225000&mode=iq&user_session_id={sid}&format=pcm-zstd&version=4&vfo=2")
    rec["iqC_status"], rec["iqC_body"] = c.status, c.fail_body
    time.sleep(2.0)
    if c.ok():
        c.close()
    d = WS("iqD", f"/ws?frequency=225000&mode=iq48&user_session_id={sid}&format=pcm-zstd&version=4&vfo=1")
    rec["iqD_status"], rec["iqD_body"] = d.status, d.fail_body              # wide IQ on a secondary: refused?
    time.sleep(2.0)
    if d.ok():
        d.close()
    if b.ok():
        tune(b, 77000)
    time.sleep(3.0)
    for pause in (2.0, 5.0):
        ev("pause_start", ws="iqA", secs=pause)
        a.paused = True
        time.sleep(pause)
        a.paused = False
        ev("pause_end", ws="iqA")
        time.sleep(4.0)
    for w in (a, b):
        w.close()


def run_lvl2(sid):
    a = iq_ws("iqA", sid, TDF - 6000)
    time.sleep(1.0)
    sp = spec_ws(sid)
    time.sleep(4.0)
    a.send({"type": "get_status"})
    for label, f, bw in (("tdf_5Hz", TDF, 5.0), ("tdf_20Hz", TDF, 20.0), ("tdf_100Hz", TDF, 100.0)):
        ev("view", label=label, f=f, bw=bw)
        sp.send({"type": "zoom", "frequency": int(f), "binBandwidth": bw})
        time.sleep(4.0)
    for w in (a, sp):
        w.close()


def run_final(sid):
    allowed = rec.get("allowed_iq_modes", [])
    a = iq_ws("iqA", sid, TDF - 6000)
    time.sleep(4.0)
    b = WS("iqB", f"/ws?frequency={int(TDF - 1500)}&mode=iq&user_session_id={sid}&format=pcm-zstd&version=4&vfo=1")
    time.sleep(3.0)
    base = TDF - 6000
    for i in range(20):                                   # primary retunes, 1.2 s apart
        tune(a, base + (2000.0 if i % 2 == 0 else 0.0))
        time.sleep(1.2)
    if b.ok():
        sb = TDF - 1500
        for i in range(6):                                # secondary retunes in plain iq, +-1 kHz
            tune(b, sb + (1000.0 if i % 2 == 0 else 0.0), mode="iq")
            time.sleep(1.5)
        b.close()
    time.sleep(1.0)
    for m in ("iq96", "iq192"):                           # wide IQ: new primary replaces the old one
        if m not in allowed:
            ev("skip", mode=m); continue
        w = iq_ws(m, sid, TDF - 6000, mode=m)
        time.sleep(8.0)
        w.close()
        time.sleep(1.0)
    a.close()


def main():
    sid = str(uuid.uuid4())
    rec["sid"] = sid
    if not admit(sid):
        return
    {"vfo": run_vfo, "ret": run_ret, "lvl": run_lvl, "vfo2": run_vfo2, "lvl2": run_lvl2, "final": run_final}[MODE](sid)


try:
    main()
except Exception as e:
    rec["fatal"] = f"{type(e).__name__}: {e}"
    print("fatal:", rec["fatal"], flush=True)
time.sleep(0.5)
rec["frames"] = {k: v.frames for k, v in rec["sockets"].items()}
rec["closed"] = {k: v.closed for k, v in rec["sockets"].items()}
rec["sockets"] = {k: v.status for k, v in rec["sockets"].items()}
rec["duration_s"] = round(now(), 2)
pickle.dump(rec, open(OUTF, "wb"))
print("saved", OUTF, {k: len(v) for k, v in rec["frames"].items()}, "dur", rec["duration_s"], flush=True)
os._exit(0)
