"""KiwiSDR protocol probe: SND + W/F behaviour measurements (K1-K13).

Standalone, stdlib + numpy. Writes a JSON-lines event log and a summary.
Usage: python -I kiwiprobe.py <host> <port> <outdir> <test>
"""
import base64, json, os, socket, ssl, struct, sys, threading, time, urllib.parse, urllib.request
import numpy as np

HOST, PORT, OUT = sys.argv[1], int(sys.argv[2]), sys.argv[3]
TEST = sys.argv[4]
SCHEME = sys.argv[5] if len(sys.argv) > 5 else "https"
TLS = SCHEME == "https"
IDENT = "sdriak-test"
os.makedirs(OUT, exist_ok=True)
T0 = time.monotonic()
LOG = open(os.path.join(OUT, f"{TEST}_{HOST}_{PORT}.jsonl"), "w", encoding="utf-8")
LOGLOCK = threading.Lock()


def now():
    return time.monotonic() - T0


def log(kind, **kw):
    kw["t"] = round(now(), 4)
    kw["kind"] = kind
    with LOGLOCK:
        LOG.write(json.dumps(kw) + "\n")
        LOG.flush()


# ---------------------------------------------------------------- websocket
class WS:
    def __init__(self, path, tls=True, timeout=10):
        raw = socket.create_connection((HOST, PORT), timeout)
        if tls:
            raw = ssl.create_default_context().wrap_socket(raw, server_hostname=HOST)
        self.sock = raw
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = self.sock.recv(4096)
            if not d:
                raise EOFError("closed during handshake")
            buf += d
        hdr, self.buf = buf.split(b"\r\n\r\n", 1)
        self.status = hdr.split(b"\r\n")[0].decode(errors="replace")
        if " 101 " not in self.status:
            raise ConnectionError(self.status)
        self.sock.settimeout(None)
        self.lock = threading.Lock()
        self.pongs = {}

    def ping(self, tag):
        self.send(tag.encode(), 9)
        return time.monotonic()

    def _recvn(self, n):
        while len(self.buf) < n:
            d = self.sock.recv(65536)
            if not d:
                raise EOFError("socket closed")
            self.buf += d
        r, self.buf = self.buf[:n], self.buf[n:]
        return r

    def send(self, payload, op=1):
        if isinstance(payload, str):
            payload = payload.encode()
        mask = os.urandom(4)
        n = len(payload)
        h = bytes([0x80 | op])
        if n < 126:
            h += bytes([0x80 | n])
        elif n < 65536:
            h += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            h += bytes([0x80 | 127]) + struct.pack(">Q", n)
        m = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        with self.lock:
            self.sock.sendall(h + mask + m)

    def recv(self):
        msg, op0 = b"", None
        while True:
            b0, b1 = self._recvn(2)
            fin, op, n = b0 & 0x80, b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._recvn(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._recvn(8))[0]
            d = self._recvn(n)
            if op == 9:
                self.send(d, 10)
                continue
            if op == 10:
                self.pongs[d.decode(errors="replace")] = time.monotonic()
                continue
            if op == 8:
                raise EOFError("ws close " + d[:2].hex() + " " + d[2:].decode(errors="replace"))
            if op != 0:
                op0 = op
            msg += d
            if fin:
                return op0, msg

    def close(self):
        try:
            self.send(b"\x03\xe8", 8)
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


# ---------------------------------------------------------------- kiwi conn
class Kiwi:
    def __init__(self, stream, ts, prefix="", keepalive=True, tls=None):
        tls = TLS if tls is None else tls
        self.paused = False
        self.load_cfg = None
        self.stream = stream            # "SND" or "W/F"
        self.name = f"{prefix or 'krec'}:{stream}"
        path = f"/{prefix + '/' if prefix else ''}{ts}/{stream}"
        self.msgs = {}
        self.closed = None
        self.keepalive = keepalive
        self.snd = []                   # (t, flags, seq, rssi, gps tuple, nsamp, offset into iq)
        self.iq = []                    # list of complex arrays
        self.wf = []                    # (t, x_bin, zoom, flags, seq, bins array)
        self.sample_rate = None
        self.audio_rate = None
        self.on_setup = None
        log("connect", conn=self.name, path=path)
        self.ws = WS(path, tls=tls)
        log("connected", conn=self.name, status=self.ws.status)
        self.send("SET auth t=kiwi p=")
        self.send(f"SERVER DE CLIENT {IDENT} {stream}")
        self.send(f"SET ident_user={IDENT}")
        self.th = threading.Thread(target=self._rx, daemon=True)
        self.th.start()
        self.ka = threading.Thread(target=self._ka, daemon=True)
        self.ka.start()

    def send(self, s):
        log("send", conn=self.name, cmd=s)
        try:
            self.ws.send(s)
        except Exception as e:
            log("send_fail", conn=self.name, err=str(e))

    def _ka(self):
        while self.closed is None:
            time.sleep(5)
            if self.keepalive and self.closed is None:
                try:
                    self.ws.send("SET keepalive")
                except Exception:
                    return

    def _rx(self):
        try:
            while True:
                while self.paused:
                    time.sleep(0.01)
                op, m = self.ws.recv()
                t = now()
                tag = m[:3]
                if tag == b"MSG":
                    self._msg(m[4:].decode(errors="replace"), t)
                elif tag == b"SND":
                    self._snd(m, t)
                elif tag == b"W/F":
                    self._wf(m, t)
                else:
                    log("other", conn=self.name, tag=tag.decode(errors="replace"), n=len(m), op=op)
        except Exception as e:
            self.closed = (now(), str(e))
            log("closed", conn=self.name, why=str(e))

    def _msg(self, body, t):
        for pair in body.split(" "):
            if "=" in pair:
                k, v = pair.split("=", 1)
            else:
                k, v = pair, None
            if k == "load_cfg" and v:
                self.load_cfg = v
            if v is not None and len(v) > 300:
                v = v[:300] + "..."
            self.msgs.setdefault(k, []).append((round(t, 4), v))
            if k not in ("load_cfg", "load_dxcfg", "load_dxcomm_cfg", "extint_list_json", "cfg_loaded"):
                log("msg", conn=self.name, k=k, v=v)
            if k == "audio_rate":
                self.audio_rate = int(v)
                self.send(f"SET AR OK in={v} out=48000")
            elif k == "sample_rate":
                self.sample_rate = float(v)
                if self.on_setup:
                    self.on_setup(self)
            elif k == "wf_setup":
                if self.on_setup:
                    self.on_setup(self)

    def _snd(self, m, t):
        flags = m[3]
        seq = struct.unpack("<I", m[4:8])[0]
        sm = struct.unpack(">H", m[8:10])[0]
        rssi = 0.1 * sm - 127
        gps = None
        data = m[10:]
        if flags & 0x08:
            lgs, _d, gs, gns = struct.unpack("<BBII", m[10:20])
            gps = (lgs, gs, gns)
            data = m[20:]
            dt = "<i2" if flags & 0x80 else ">i2"
            s = np.frombuffer(data, dtype=dt).astype(np.float32) / 32768.0
            c = s[0::2] + 1j * s[1::2]
        else:
            c = np.zeros(0, dtype=np.complex64)
        off = sum(len(x) for x in self.iq)
        self.iq.append(c.astype(np.complex64))
        self.snd.append((t, flags, seq, rssi, gps, len(c), off, len(m)))

    def _wf(self, m, t):
        body = m[4:]
        xb, fz, seq = struct.unpack("<III", body[0:12])
        bins = np.frombuffer(body[12:], dtype=np.uint8).copy()
        self.wf.append((t, xb, fz & 0xFFFF, fz >> 16, seq, bins))

    def close(self):
        self.ws.close()
        if self.closed is None:
            self.closed = (now(), "client close")


def users():
    req = urllib.request.Request(f"{SCHEME}://{HOST}:{PORT}/users", headers={"User-Agent": IDENT})
    with urllib.request.urlopen(req, timeout=10) as r:
        u = json.loads(r.read())
    return [(x["i"], urllib.parse.unquote(x.get("n", ""))) for x in u if "n" in x]


def get_ts():
    req = urllib.request.Request(f"{SCHEME}://{HOST}:{PORT}/VER", headers={"User-Agent": IDENT})
    with urllib.request.urlopen(req, timeout=10) as r:
        return int(json.loads(r.read())["ts"])


def snd_setup(freq_khz):
    def f(k):
        k.send("SET compression=0")
        k.send("SET agc=0 hang=0 thresh=-100 slope=6 decay=1000 manGain=50")
        k.send(f"SET mod=iq low_cut=-5900 high_cut=5900 freq={freq_khz:.3f}")
        k.send("SET keepalive")
    return f


def wf_setup(zoom, cf_khz):
    def f(k):
        k.send("SET wf_speed=4")
        k.send("SET interp=13")
        k.send("SET wf_comp=0")
        k.send("SET maxdb=-10 mindb=-110")
        k.send(f"SET zoom={zoom} cf={cf_khz:.3f}")
        k.send("SET keepalive")
    return f


def wait_for(pred, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.05)
    return False


def summary(name, obj):
    with open(os.path.join(OUT, f"{TEST}_{HOST}_{PORT}_summary.json"), "a", encoding="utf-8") as fp:
        fp.write(json.dumps({name: obj}, default=str) + "\n")
    print(name, json.dumps(obj, default=str, ensure_ascii=False)[:4000], flush=True)


def first(k, key):
    v = k.msgs.get(key)
    return v[0][1] if v else None


SETUP_KEYS = ["rx_chan", "wf_chans", "wf_chans_real", "rx_chans", "zoom_max", "wf_cal", "bandwidth",
              "center_freq", "audio_rate", "sample_rate", "too_busy", "badp", "redirect", "version_maj",
              "version_min", "freq_offset", "wf_fps", "wf_fps_max", "wf_fft_size", "monitor", "camp"]


def setup_report(k):
    return {key: first(k, key) for key in SETUP_KEYS if key in k.msgs}


# ---------------------------------------------------------------- analysis helpers
def flags_hist(k):
    h = {}
    for p in k.snd:
        h[hex(p[1])] = h.get(hex(p[1]), 0) + 1
    return h


def wf_db(bins, wf_cal):
    return bins.astype(np.int32) - 255 + wf_cal


def wf_geom(xb, zoom, zoom_max, bw):
    hz_per_start = bw / (1024 << zoom_max)
    start = xb * hz_per_start
    span = bw / (1 << zoom)
    return start, span


def inst_freq(c, sr, smooth=48):
    d = np.angle(c[1:] * np.conj(c[:-1])) * sr / (2 * np.pi)
    k = np.ones(smooth) / smooth
    return np.convolve(d, k, mode="same")


def all_iq(k):
    return np.concatenate(k.iq) if k.iq else np.zeros(0, np.complex64)


def snd_seq_at(k, t):
    last = None
    for p in k.snd:
        if p[0] <= t:
            last = p[2]
        else:
            break
    return last


# ---------------------------------------------------------------- tests
def t_plainws():
    ts = get_ts()
    try:
        w = WS(f"/{ts}/SND", tls=False)
        summary("plain_ws", {"status": w.status})
        w.close()
    except Exception as e:
        summary("plain_ws", {"error": str(e)})


def t_k1():
    out = {}
    for prefix in ["", "kiwi"]:
        ts = get_ts()
        k = Kiwi("SND", ts, prefix=prefix)
        k.on_setup = snd_setup(7000.0)
        wait_for(lambda: len(k.snd) > 100 or k.closed, 12)
        time.sleep(1)
        out[prefix or "krec"] = {"users": users(), "setup": setup_report(k), "packets": len(k.snd), "flags": flags_hist(k),
                                 "closed": k.closed, "msg_keys": sorted(k.msgs.keys())}
        k.close()
        time.sleep(4)
    summary("K1_url_forms", out)


def t_k3():
    ts = get_ts()
    s = Kiwi("SND", ts)
    s.on_setup = snd_setup(7000.0)
    wait_for(lambda: s.sample_rate is not None, 10)
    time.sleep(2)
    try:
        w = Kiwi("W/F", ts)
        w.on_setup = wf_setup(0, 15000)
        wait_for(lambda: len(w.wf) > 20 or w.closed, 10)
        time.sleep(1)
        res = {"users": users(), "snd_setup": setup_report(s), "wf_setup": setup_report(w), "wf_frames": len(w.wf),
               "wf_closed": w.closed, "wf_msg_keys": sorted(w.msgs.keys())}
        w.close()
    except Exception as e:
        res = {"snd_setup": setup_report(s), "wf_connect_error": str(e)}
    res["snd_packets"] = len(s.snd)
    res["snd_closed_after"] = s.closed
    s.close()
    summary("K3_snd_first_then_wf", res)


def t_main():
    ts = get_ts()
    w = Kiwi("W/F", ts)
    w.on_setup = wf_setup(0, 15000)
    wait_for(lambda: "wf_setup" in w.msgs or w.closed, 10)
    s = Kiwi("SND", ts)
    s.on_setup = snd_setup(7000.0)
    wait_for(lambda: s.sample_rate is not None and len(s.snd) > 20, 12)
    wait_for(lambda: len(w.wf) > 10, 5)
    summary("K2_K4_pairing_setup", {"users": users(), "wf_setup": setup_report(w), "snd_setup": setup_report(s),
                                    "wf_frames": len(w.wf), "snd_packets": len(s.snd),
                                    "wf_closed": w.closed, "snd_closed": s.closed})
    if w.closed or s.closed:
        return
    zoom_max = int(first(w, "zoom_max"))
    bw = float(first(w, "bandwidth") or first(s, "bandwidth") or 30e6)
    wf_cal = int(first(w, "wf_cal") or -13)
    sr = s.sample_rate

    def setwf(z, cf_khz, settle):
        tsend = now()
        w.send(f"SET zoom={z} cf={cf_khz:.3f}")
        time.sleep(settle)
        return tsend

    def frames_since(t0):
        return [f for f in w.wf if f[0] >= t0]

    # --- carrier search
    cands = [77.5, 198.0, 225.0, 183.0, 153.0, 162.0, 252.0, 279.0]
    best = None
    scan = {}
    for c in cands:
        t0 = setwf(11, c, 1.8)
        fr = [f for f in frames_since(t0 + 0.6) if f[2] == 11]
        if not fr:
            continue
        b = np.median(np.stack([wf_db(f[5], wf_cal) for f in fr]), axis=0)
        start, span = wf_geom(fr[-1][1], 11, zoom_max, bw)
        freqs = start + (np.arange(1024) + 0.5) * span / 1024
        sel = np.abs(freqs - c * 1e3) < 300
        if not sel.any():
            continue
        pk = int(np.argmax(np.where(sel, b, -999)))
        snr = float(b[pk] - np.median(b))
        scan[c] = {"peak_dBm": float(b[pk]), "snr_dB": snr, "peak_hz": float(freqs[pk])}
        if best is None or snr > best[1]:
            best = (c, snr, float(freqs[pk]))
    summary("carrier_scan", scan)
    if best is None:
        return
    t0 = setwf(zoom_max, best[2] / 1e3, 5.0)
    fr = [f for f in frames_since(t0 + 2.5) if f[2] == zoom_max]
    fc = best[2]
    if fr:
        b = np.median(np.stack([wf_db(f[5], wf_cal) for f in fr]), axis=0)
        start, span = wf_geom(fr[-1][1], zoom_max, zoom_max, bw)
        freqs = start + (np.arange(1024) + 0.5) * span / 1024
        fc = float(freqs[int(np.argmax(b))])
    summary("carrier", {"nominal_khz": best[0], "zoom11_snr": best[1], "fc_hz_wf": fc})

    # --- K5: NEW_FREQ retune marker
    base = fc - 1500.0
    s.send(f"SET mod=iq low_cut=-5900 high_cut=5900 freq={base / 1e3:.3f}")
    time.sleep(3.0)
    retunes = []
    for i in range(8):
        f = base + (2000.0 if i % 2 == 0 else 0.0)
        t_send = now()
        npk_before = len(s.snd)
        s.send(f"SET mod=iq low_cut=-5900 high_cut=5900 freq={f / 1e3:.3f}")
        retunes.append((t_send, npk_before, f))
        time.sleep(2.5)
    # K11 level comparison: carrier at +1500 Hz in IQ, WF at zoom_max on fc
    t0 = setwf(zoom_max, fc / 1e3, 4.0)
    fr = [f for f in frames_since(t0 + 2.0) if f[2] == zoom_max]
    rssi = [p[3] for p in s.snd if p[0] >= t0 + 2.0]
    if fr:
        b = np.stack([wf_db(f[5], wf_cal) for f in fr])
        summary("K11_levels", {"wf_peak_dBm_median": float(np.median(b.max(axis=1))),
                               "wf_noise_dBm_median": float(np.median(b)),
                               "snd_rssi_median": float(np.median(rssi)) if rssi else None,
                               "wf_cal": wf_cal})
    iq = all_iq(s)
    fi = inst_freq(iq, sr)
    res = []
    for (t_send, npk, f) in retunes:
        expect = fc - f
        prev = fc - (base if f != base else base + 2000.0)
        nf = None
        for p in s.snd[npk:]:
            if p[1] & 0x04:
                nf = p
                break
        if npk >= len(s.snd):
            continue
        i0 = s.snd[npk][6]
        seg = fi[i0:i0 + int(sr * 2.0)]
        near = np.abs(seg - expect) < 120
        jump = None
        for j in range(len(near) - 300):
            if near[j:j + 300].all():
                jump = i0 + j
                break
        res.append({"expect_hz": expect, "prev_hz": prev,
                    "newfreq_pkt_seq": nf[2] if nf else None,
                    "newfreq_pkt_start_sample": nf[6] if nf else None,
                    "newfreq_arrival_after_send_ms": round((nf[0] - t_send) * 1e3, 1) if nf else None,
                    "jump_sample": jump,
                    "jump_minus_newfreq_start": (jump - nf[6]) if (jump is not None and nf) else None,
                    "pkts_between_send_and_newfreq": (s.snd.index(nf) - npk) if nf else None})
    summary("K5_new_freq", res)

    # --- K8: geometry, clamping, echo
    geo = {}
    for (z, cf) in [(3, 100.0), (3, 29950.0), (0, 15000.0), (5, fc / 1e3), (10, fc / 1e3),
                    (zoom_max, fc / 1e3), (2, fc / 1e3)]:
        nmsg = len(w.msgs.get("zoom", []))
        t0 = setwf(z, cf, 3.5)
        echo = w.msgs.get("zoom", [])[nmsg:]
        echo_start = w.msgs.get("start", [])
        fr = frames_since(t0)
        first_new = next((f for f in fr if f[2] == z), None)
        old_after_echo = sum(1 for f in fr if echo and f[0] > echo[0][0] and f[2] != z)
        last = fr[-1] if fr else None
        g = {}
        if last:
            start, span = wf_geom(last[1], last[2], zoom_max, bw)
            g = {"x_bin": last[1], "zoom": last[2], "start_hz": start, "span_hz": span,
                 "centre_hz": start + span / 2, "requested_cf_hz": cf * 1e3}
        geo[f"z{z}_cf{cf:.1f}"] = {
            "echo_ms": round((echo[0][0] - t0) * 1e3, 1) if echo else None,
            "echo_start": echo_start[-1][1] if echo_start else None,
            "first_new_frame_ms": round((first_new[0] - t0) * 1e3, 1) if first_new else None,
            "frames_old_geometry_after_echo": old_after_echo,
            "frames": len(fr), "last": g}
    summary("K8_geometry", geo)

    # --- K10: frame rates
    rates = {}
    for z in [0, zoom_max]:
        setwf(z, (15000.0 if z == 0 else fc / 1e3), 1.0)
        for sp in [1, 2, 3, 4]:
            w.send(f"SET wf_speed={sp}")
            time.sleep(0.8)
            t0 = now()
            time.sleep(8.0 if sp == 1 else 5.0)
            fr = [f for f in w.wf if t0 <= f[0] < now()]
            rates[f"z{z}_speed{sp}"] = round(len(fr) / (now() - t0), 2)
    w.send("SET wf_speed=4")
    summary("K10_frame_rates", rates)

    # --- K9: wf seq vs snd seq at arrival
    lag = {}
    for f in w.wf:
        cur = snd_seq_at(s, f[0])
        if cur is None:
            continue
        lag.setdefault(f[2], []).append(cur - f[4])
    summary("K9_seq_lag_packets", {z: {"median": float(np.median(v)), "min": int(min(v)), "max": int(max(v)),
                                       "n": len(v)} for z, v in lag.items()})

    # --- K12: W/F without keepalive (SND keeps its keepalive)
    w.keepalive = False
    t0 = now()
    wait_for(lambda: w.closed is not None, 80)
    summary("K12_wf_no_keepalive", {"closed": w.closed, "waited_s": round(now() - t0, 1),
                                    "snd_closed": s.closed})

    # --- K6, K7, K13 over the whole SND session
    t = np.array([p[0] for p in s.snd])
    cum = np.cumsum([p[5] for p in s.snd])
    fit = np.polyfit(t, cum, 1)
    seqs = [p[2] for p in s.snd]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)
    gps = [p[4] for p in s.snd if p[4]]
    summary("K6_K7_K13", {"audio_rate": s.audio_rate, "sample_rate_msg": sr,
                          "measured_rate": float(fit[0]), "ppm_vs_msg": float((fit[0] / sr - 1) * 1e6),
                          "ppm_msg_vs_12000": float((sr / 12000.0 - 1) * 1e6) if sr else None,
                          "duration_s": float(t[-1] - t[0]), "packets": len(s.snd),
                          "samples_per_packet": sorted(set(p[5] for p in s.snd)),
                          "packet_bytes": sorted(set(p[7] for p in s.snd)),
                          "flags_hist": flags_hist(s),
                          "dropped_by_eq_0x08_check": sum(1 for p in s.snd if p[1] != 0x08),
                          "seq_gaps": gaps,
                          "gps_samples": gps[:3] + gps[-3:], "gps_nonzero": sum(1 for g in gps if g[1])})
    s.close()
    if w.closed is None:
        w.close()


def t_main2():
    fc_nom = 162000.0                       # TDF Allouis, strongest in the main run's scan
    ts = get_ts()
    w = Kiwi("W/F", ts)
    w.on_setup = wf_setup(11, fc_nom / 1e3)
    wait_for(lambda: "wf_setup" in w.msgs or w.closed, 10)
    s = Kiwi("SND", ts)
    base = fc_nom - 1500.0
    s.on_setup = snd_setup(base / 1e3)
    wait_for(lambda: s.sample_rate is not None and len(s.snd) > 20, 12)
    sr = s.sample_rate
    wf_cal = int(first(w, "wf_cal") or -13)
    zoom_max = int(first(w, "zoom_max"))
    bw = float(first(w, "bandwidth") or 30e6)

    # K6: quiet stream, only keepalives; arrival-rate regression
    t_q0 = now()
    n_q0 = len(s.snd)
    time.sleep(150)
    q = s.snd[n_q0:]
    t = np.array([p[0] for p in q])
    cum = np.cumsum([p[5] for p in q])
    fit = np.polyfit(t, cum, 1)
    resid = cum - np.polyval(fit, t)
    first_pk = s.snd[:40]
    burst = [round(p[0] - s.snd[0][0], 3) for p in first_pk[::4]]
    summary("K6_quiet", {"sample_rate_msg": sr, "measured_rate": float(fit[0]),
                         "ppm_vs_msg": float((fit[0] / sr - 1) * 1e6), "duration_s": float(t[-1] - t[0]),
                         "resid_samples_min_max": [float(resid.min()), float(resid.max())],
                         "first_40_arrivals_every4_s": burst})

    # K11 + bin convention: carrier at +1500 Hz in IQ, WF zoom 11 on the carrier
    t0 = now()
    time.sleep(4)
    fr = [f for f in w.wf if f[0] >= t0 + 1 and f[2] == 11]
    iq = all_iq(s)
    seg = iq[-int(sr * 3):]
    spec = np.abs(np.fft.fftshift(np.fft.fft(seg * np.hanning(len(seg))))) ** 2
    fr_ax = np.fft.fftshift(np.fft.fftfreq(len(seg), 1 / sr))
    f_iq = float(fr_ax[int(np.argmax(spec))])
    fc_iq = base + f_iq
    rssi = [p[3] for p in s.snd if p[0] >= t0 + 1]
    res = {"carrier_hz_from_iq": fc_iq, "snd_rssi_median": float(np.median(rssi))}
    if fr:
        b = np.stack([wf_db(f[5], wf_cal) for f in fr])
        start, span = wf_geom(fr[-1][1], 11, zoom_max, bw)
        pk = int(np.argmax(np.median(b, axis=0)))
        binw = span / 1024
        res.update({"wf_peak_dBm_median": float(np.median(b.max(axis=1))), "wf_noise_dBm_median": float(np.median(b)),
                    "wf_bin_width_hz": binw, "wf_peak_bin": pk,
                    "bin_left_edge_hz": start + pk * binw, "bin_centre_hz": start + (pk + 0.5) * binw,
                    "carrier_minus_left_edge_bins": (fc_iq - (start + pk * binw)) / binw})
    summary("K11_levels_bins", res)

    # K5 detail: save IQ around four retunes
    marks = []
    for i in range(4):
        f = base + (2000.0 if i % 2 == 0 else 0.0)
        npk = len(s.snd)
        s.send(f"SET mod=iq low_cut=-5900 high_cut=5900 freq={f / 1e3:.3f}")
        time.sleep(2.0)
        nf = next((p for p in s.snd[npk:] if p[1] & 0x04), None)
        if nf:
            marks.append(nf[6])
    iq = all_iq(s)
    np.savez(os.path.join(OUT, "k5_iq.npz"), iq=iq, marks=np.array(marks), sr=sr, base=base, fc=fc_iq)
    summary("K5_saved", {"marks": marks, "n": len(iq)})
    s.close()
    w.close()


def t_bins():
    fc = 162000.36                           # TDF carrier measured from IQ in main2
    ts = get_ts()
    w = Kiwi("W/F", ts)
    w.on_setup = wf_setup(5, fc / 1e3)
    wait_for(lambda: "wf_setup" in w.msgs or w.closed, 10)
    zoom_max = int(first(w, "zoom_max"))
    bw = float(first(w, "bandwidth") or 30e6)
    out = {}
    for z, cf in [(5, 470.0), (7, 160.0), (8, 150.0), (9, 165.0), (10, 158.0), (11, 162.0), (11, 159.0)]:
        t0 = now()
        w.send(f"SET zoom={z} cf={cf:.3f}")
        time.sleep(2.5)
        fr = [f for f in w.wf if f[0] >= t0 + 1.0 and f[2] == z]
        if not fr:
            out[f"z{z}_cf{cf}"] = None
            continue
        b = np.median(np.stack([f[5].astype(int) for f in fr]), axis=0)
        start, span = wf_geom(fr[-1][1], z, zoom_max, bw)
        binw = span / 1024
        pk = int(np.argmax(b))
        out[f"z{z}_cf{cf}"] = {"bin_w_hz": round(binw, 3), "peak_bin": pk,
                               "carrier_pos_bins_from_start": round((fc - start) / binw, 3),
                               "peak_minus_pos": round(pk - (fc - start) / binw, 3),
                               "neighbour_dB": [int(b[pk - 1] - 255), int(b[pk] - 255), int(b[pk + 1] - 255)]}
    summary("bin_convention", out)
    w.close()


if __name__ == "__main__" and not os.environ.get("KIWIPROBE_LIB"):
    globals()["t_" + TEST]()
    LOG.close()
    sys.exit(0)
    {"plainws": t_plainws, "k1": t_k1, "k3": t_k3, "main": t_main, "main2": t_main2}[TEST]()
    LOG.close()
