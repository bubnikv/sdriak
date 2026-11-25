"""Second KiwiSDR test batch (A1-A6, B1-B5, C1), one server per process.
Usage: python kiwi_tests.py <test> <scheme> <host> <port>"""
import os, sys, time, json, urllib.parse, ssl, socket, http.client

test, scheme, host, port = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
sys.argv = ["kiwiprobe", host, port, "out2", test, scheme]
os.environ["KIWIPROBE_LIB"] = "1"
sys.stdout.reconfigure(encoding="utf-8")
import kiwiprobe as kp
_users = kp.users
def _safe_users():
    try:
        return _users()
    except Exception as e:
        return f"users n/a ({type(e).__name__})"
kp.users = _safe_users
import numpy as np

CANDS = [77.5, 162.0, 198.0, 225.0, 252.0, 153.0, 5000.0, 10000.0, 15000.0, 3330.0, 7850.0, 14670.0, 2500.0]


def status():
    c = (http.client.HTTPSConnection(host, int(port), timeout=10, context=ssl.create_default_context()) if scheme == "https"
         else http.client.HTTPConnection(host, int(port), timeout=10))
    c.request("GET", "/status", headers={"User-Agent": kp.IDENT})
    d = dict(l.split("=", 1) for l in c.getresponse().read(8192).decode("utf-8", "replace").splitlines() if "=" in l)
    return d


def pair(zoom=0, cf=15000.0, snd_khz=7000.0):
    ts = kp.get_ts()
    w = kp.Kiwi("W/F", ts)
    w.on_setup = kp.wf_setup(zoom, cf)
    kp.wait_for(lambda: "wf_setup" in w.msgs or w.closed, 10)
    s = kp.Kiwi("SND", ts)
    s.on_setup = kp.snd_setup(snd_khz)
    kp.wait_for(lambda: (s.sample_rate is not None and len(s.snd) > 10) or s.closed, 12)
    return w, s


def geom(w):
    return int(kp.first(w, "zoom_max") or 14), int(kp.first(w, "zoom_cap") or kp.first(w, "zoom_max") or 14), \
        float(kp.first(w, "bandwidth") or 30e6), int(kp.first(w, "wf_cal") or -13)


def find_carrier(w, zoom):
    zoom_max, cap, bw, cal = geom(w)
    best, scan = None, {}
    for c in CANDS:
        if c * 1e3 > bw:
            continue
        t0 = kp.now()
        w.send(f"SET zoom={zoom} cf={c:.3f}")
        time.sleep(1.8)
        fr = [f for f in w.wf if f[0] >= t0 + 0.6 and f[2] == zoom]
        if not fr:
            continue
        b = np.median(np.stack([kp.wf_db(f[5], cal) for f in fr]), axis=0)
        start, span = kp.wf_geom(fr[-1][1], zoom, zoom_max, bw)
        freqs = start + np.arange(1024) * span / 1024
        sel = np.abs(freqs - c * 1e3) < 200
        if not sel.any():
            continue
        pk = int(np.argmax(np.where(sel, b, -999)))
        snr = float(b[pk] - np.median(b))
        scan[c] = round(snr, 1)
        if best is None or snr > best[1]:
            best = (c, snr, float(freqs[pk] + span / 2048))
    return best, scan


def switch_offsets(s, marks, fc, bases, sr):
    iq = kp.all_iq(s)
    fi = kp.inst_freq(iq, sr, 24)
    out = []
    for m, (old_lo, new_lo) in zip(marks, bases):
        old, new = fc - old_lo, fc - new_lo
        w = fi[m - 1024:m + 2048]
        on_old = np.nonzero(np.abs(w - old) < 60)[0]
        on_new = np.abs(w - new) < 60
        first_new = next((j - 1024 for j in range(len(on_new) - 200) if on_new[j:j + 200].all()), None)
        out.append({"last_old": int(on_old.max() - 1024) if len(on_old) else None, "first_new": first_new})
    return out


def t_a1():
    st = status()
    res = {"status_users": f"{st.get('users')}/{st.get('users_max')}", "mode": st.get("mode"), "occupied": kp.users()}
    ts = kp.get_ts()
    w = kp.Kiwi("W/F", ts)
    w.on_setup = kp.wf_setup(0, 15000)
    kp.wait_for(lambda: len(w.wf) > 10 or w.closed, 8)
    res["wf_first"] = {"setup": kp.setup_report(w), "frames": len(w.wf), "closed": w.closed,
                       "users_during": kp.users()}
    w.close()
    time.sleep(12)
    ts = kp.get_ts()
    s = kp.Kiwi("SND", ts)
    s.on_setup = kp.snd_setup(7000.0)
    kp.wait_for(lambda: s.sample_rate is not None or s.closed, 10)
    time.sleep(1.5)
    w2 = kp.Kiwi("W/F", ts)
    w2.on_setup = kp.wf_setup(0, 15000)
    kp.wait_for(lambda: len(w2.wf) > 10 or w2.closed, 8)
    res["snd_then_wf"] = {"users_during": kp.users(), "snd_packets": len(s.snd), "wf_setup": kp.setup_report(w2),
                          "wf_frames": len(w2.wf), "wf_closed": w2.closed}
    w2.close()
    s.close()
    kp.summary("A1_wf_refusal", res)


def t_a2():
    st = status()
    res = {"status_users": f"{st.get('users')}/{st.get('users_max')}"}
    ts = kp.get_ts()
    try:
        s = kp.Kiwi("SND", ts)
        s.on_setup = kp.snd_setup(7000.0)
        kp.wait_for(lambda: len(s.snd) > 5 or s.closed, 8)
        res.update({"msgs": {k: s.msgs[k][0][1] for k in ("too_busy", "monitor", "badp", "camp", "redirect", "down")
                             if k in s.msgs},
                    "packets": len(s.snd), "closed": s.closed})
        s.close()
    except Exception as e:
        res["error"] = str(e)
    kp.summary("A2_krec_on_full", res)


def t_a3():
    """A3 switch offset, A4 fast retunes, A6 back-pressure, B5 limits; rx3.wf3 server."""
    w, s = pair(0, 15000.0)
    if w.closed or s.closed:
        kp.summary("A3_failed", {"w": w.closed, "s": s.closed, "w_setup": kp.setup_report(w)})
        return
    zoom_max, cap, bw, cal = geom(w)
    sr = s.sample_rate
    best, scan = find_carrier(w, min(cap, 10))
    kp.summary("A3_carrier", {"best": best, "scan": scan, "sample_rate": sr, "audio_rate": s.audio_rate})
    if not best or best[1] < 25:
        s.close(); w.close(); return
    fc = best[2]
    base = fc - 1500.0
    s.send(f"SET mod=iq low_cut=-5000 high_cut=5000 freq={base / 1e3:.3f}")
    time.sleep(2.5)
    marks, bases = [], []
    cur = base
    for i in range(5):
        nxt = base + (2000.0 if i % 2 == 0 else 0.0)
        npk = len(s.snd)
        t_send = kp.now()
        s.send(f"SET mod=iq low_cut=-5000 high_cut=5000 freq={nxt / 1e3:.3f}")
        time.sleep(2.0)
        nf = next((p for p in s.snd[npk:] if p[1] & 0x04), None)
        if nf:
            marks.append(nf[6]); bases.append((cur, nxt))
        cur = nxt
    kp.summary("A3_switch_offset", {"packet_samples": sorted(set(p[5] for p in s.snd)),
                                    "offsets": switch_offsets(s, marks, fc, bases, sr)})
    # A4: 20 retunes at ~15/s
    npk = len(s.snd)
    t0 = kp.now()
    seq_f = []
    for i in range(20):
        f = base + (2000.0 if i % 2 == 0 else 0.0)
        seq_f.append(f)
        s.send(f"SET mod=iq low_cut=-5000 high_cut=5000 freq={f / 1e3:.3f}")
        time.sleep(1 / 15)
    t_last = kp.now()
    time.sleep(3.0)
    pk = s.snd[npk:]
    flagged = [p for p in pk if p[1] & 0x04]
    iq = kp.all_iq(s)
    fi = kp.inst_freq(iq, sr, 48)
    final_expect = fc - seq_f[-1]
    i_last_cmd = next((p[6] for p in pk if p[0] >= t_last), None)
    settle = None
    if i_last_cmd is not None:
        seg = np.abs(fi[i_last_cmd:] - final_expect) < 80
        settle = next((j for j in range(len(seg) - 400) if seg[j:j + 400].all()), None)
    kp.summary("A4_fast_retunes", {"commands": 20, "flagged_packets": len(flagged),
                                   "flag_arrivals_s": [round(p[0] - t0, 3) for p in flagged],
                                   "packets_in_window": len(pk),
                                   "settle_samples_after_last_cmd_arrival": settle})
    # A6: back-pressure on SND only
    a6 = {}
    for pause in (2.0, 5.0):
        n0 = len(s.snd)
        s.paused = True
        tp = kp.now()
        time.sleep(pause)
        s.paused = False
        time.sleep(4.0)
        after = s.snd[n0:]
        seqs = [p[2] for p in after]
        gaps = [(a, b) for a, b in zip(seqs, seqs[1:]) if b != a + 1]
        burst = [p for p in after if p[0] - tp < pause + 0.3]
        a6[f"pause_{pause}s"] = {"packets": len(after), "seq_gaps": gaps[:5],
                                 "packets_within_0.3s_of_resume": len(burst),
                                 "expected_packets_for_window": round((pause + 4.0) * sr / max(1, after[0][5]) if after else 0, 1),
                                 "closed": s.closed}
    kp.summary("A6_backpressure", a6)
    # B5: limits from load_cfg
    lim = {}
    if s.load_cfg:
        try:
            cfg = json.loads(urllib.parse.unquote(s.load_cfg))
            def walk(d, pre=""):
                for k, v in (d.items() if isinstance(d, dict) else []):
                    kk = pre + k
                    if isinstance(v, dict):
                        walk(v, kk + ".")
                    elif any(x in k.lower() for x in ("inactiv", "limit", "tlimit", "camp", "pwd", "timeout", "kick")):
                        lim[kk] = v
            walk(cfg)
        except Exception as e:
            lim["parse_error"] = str(e)
    kp.summary("B5_limits", lim)
    s.close()
    w.close()


def t_a5():
    ts = kp.get_ts()
    w = kp.Kiwi("W/F", ts)
    w.on_setup = kp.wf_setup(8, 7100.0)
    kp.wait_for(lambda: len(w.wf) > 10 or w.closed, 8)
    if w.closed:
        kp.summary("A5_failed", {"closed": w.closed}); return
    zoom_max, cap, bw, cal = geom(w)
    time.sleep(1.0)
    nmsg = len(w.msgs.get("start", []))
    nfr = len(w.wf)
    t0 = kp.now()
    sent = []
    for i in range(30):
        cf = 7100.0 + i * 1.0
        sent.append((kp.now(), cf))
        w.send(f"SET zoom=8 cf={cf:.3f}")
        time.sleep(0.1)
    time.sleep(2.0)
    echoes = w.msgs.get("start", [])[nmsg:]
    fr = w.wf[nfr:]
    starts = [f[1] for f in fr]
    distinct = len(set(starts))
    final_cf = 7129.0e3
    hz = bw / (1024 << zoom_max)
    final_start = round((final_cf - bw / (1 << 8) / 2) / hz)
    settle = next((round(f[0] - sent[-1][0], 3) for f in fr if f[0] >= sent[-1][0] and abs(f[1] - final_start) < 2), None)
    kp.summary("A5_pan", {"commands": 30, "rate_hz": 10, "echoes": len(echoes), "frames": len(fr),
                          "distinct_frame_starts": distinct,
                          "frames_per_s": round(len(fr) / (kp.now() - t0), 1),
                          "final_settle_after_last_cmd_s": settle})
    w.close()


def t_b2():
    ts = kp.get_ts()
    w = kp.Kiwi("W/F", ts)
    w.on_setup = kp.wf_setup(0, 15000)
    kp.wait_for(lambda: "wf_setup" in w.msgs or w.closed, 10)
    time.sleep(1.5)
    zoom_max, cap, bw, cal = geom(w)
    foff = float(kp.first(w, "freq_offset") or 0) * 1e3
    res = {"freq_offset_hz": foff, "bandwidth": bw, "center_freq": kp.first(w, "center_freq")}
    for label, cf in (("rf", foff + bw / 2), ("baseband", bw / 2), ("rf_low", foff + 1e6), ("baseband_low", 1e6)):
        nmsg = len(w.msgs.get("start", []))
        t0 = kp.now()
        w.send(f"SET zoom=5 cf={cf / 1e3:.3f}")
        time.sleep(2.5)
        echo = w.msgs.get("start", [])[nmsg:]
        fr = [f for f in w.wf if f[0] >= t0 + 1.0 and f[2] == 5]
        g = None
        if fr:
            start, span = kp.wf_geom(fr[-1][1], 5, zoom_max, bw)
            g = {"start_hz": start, "centre_hz": start + span / 2}
        res[label] = {"requested_cf_hz": cf, "echo_start": echo[-1][1] if echo else None, "frame": g}
    kp.summary("B2_freq_offset", res)
    w.close()


def t_b3c1():
    w, s = pair(0, 16000.0)
    if w.closed or s.closed:
        kp.summary("B3C1_failed", {"w": w.closed, "s": s.closed}); return
    zoom_max, cap, bw, cal = geom(w)
    res = {"bandwidth_msg": kp.first(w, "bandwidth"), "zoom_max": zoom_max, "zoom_cap": cap}
    best, scan = find_carrier(w, min(cap, 9))
    res["carrier"] = best
    if best:
        # IQ-measured carrier for the bin check
        base = best[2] - 1500.0
        s.send(f"SET mod=iq low_cut=-5000 high_cut=5000 freq={base / 1e3:.3f}")
        time.sleep(3.0)
        iq = kp.all_iq(s)[-int(s.sample_rate * 2):]
        sp = np.abs(np.fft.fftshift(np.fft.fft(iq * np.hanning(len(iq))))) ** 2
        fa = np.fft.fftshift(np.fft.fftfreq(len(iq), 1 / s.sample_rate))
        fc = base + float(fa[int(np.argmax(sp))])
        z = min(cap, 9)
        t0 = kp.now()
        w.send(f"SET zoom={z} cf={fc / 1e3 + 3:.3f}")
        time.sleep(2.5)
        fr = [f for f in w.wf if f[0] >= t0 + 1.0 and f[2] == z]
        if fr:
            b = np.median(np.stack([f[5].astype(int) for f in fr]), axis=0)
            pk = int(np.argmax(b))
            for name, B in (("bw_from_msg", bw), ("bw_30MHz", 30e6)):
                start, span = kp.wf_geom(fr[-1][1], z, zoom_max, B)
                res[name] = {"pos_bins": round((fc - start) / (span / 1024), 2), "peak_bin": pk}
        res["fc_iq"] = fc
    # C1: GPS timestamps over 60 s
    n0 = len(s.snd)
    time.sleep(60)
    g = [(p[4], p[6], p[5]) for p in s.snd[n0:] if p[4]]
    gps = {"packets": len(g), "last_gps_solution": sorted(set(x[0][0] for x in g))[:5]}
    good = [(x[0][1] + x[0][2] * 1e-9, x[1]) for x in g if x[0][1]]
    if len(good) > 10:
        t = np.array([a for a, _ in good]); n = np.array([b for _, b in good], float)
        fit = np.polyfit(t - t[0], n - n[0], 1)
        resid = (n - n[0]) - np.polyval(fit, t - t[0])
        gps.update({"rate_from_gps": float(fit[0]), "sample_rate_msg": s.sample_rate,
                    "ppm": float((fit[0] / s.sample_rate - 1) * 1e6),
                    "resid_samples_rms": float(np.sqrt(np.mean(resid ** 2))),
                    "monotonic": bool(np.all(np.diff(t) > 0))})
    res["C1_gps"] = gps
    kp.summary("B3_C1", res)
    s.close()
    w.close()


def t_b1():
    st = status()
    res = {k: st.get(k) for k in ("sdr_hw", "sw_version", "mode", "users", "users_max", "name")}
    ts = kp.get_ts()
    try:
        w = kp.Kiwi("W/F", ts)
        w.on_setup = kp.wf_setup(0, 15000)
        kp.wait_for(lambda: len(w.wf) > 10 or w.closed, 8)
        res["wf"] = {"setup": kp.setup_report(w), "frames": len(w.wf), "closed": w.closed,
                     "frame_bytes": sorted(set(len(f[5]) for f in w.wf))[:3],
                     "geom": [(f[1], f[2], f[3], f[4]) for f in w.wf[:3]]}
        w.close()
    except Exception as e:
        res["wf_error"] = str(e)
    kp.summary("B1_compat", res)


def t_b4():
    res = {}
    ctx = ssl.create_default_context()
    try:
        with socket.create_connection((host, int(port)), 10) as raw:
            with ctx.wrap_socket(raw, server_hostname=host) as t:
                c = t.getpeercert()
                res = {"verified": True, "tls": t.version(), "issuer": dict(x[0] for x in c["issuer"]).get("organizationName"),
                       "notAfter": c.get("notAfter")}
    except ssl.SSLCertVerificationError as e:
        res = {"verified": False, "error": e.verify_message}
    except Exception as e:
        res = {"error": f"{type(e).__name__}: {e}"}
    kp.summary("B4_tls", res)


STEP = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
        97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
        796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
        4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767)
IDX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def ima_decode(data):
    index, prev, out = 0, 0, []
    for b in data:
        for code in (b & 0x0F, b >> 4):
            step = STEP[index]
            index = min(max(index + IDX[code], 0), len(STEP) - 1)
            diff = step >> 3
            if code & 1: diff += step >> 2
            if code & 2: diff += step >> 1
            if code & 4: diff += step
            if code & 8: diff = -diff
            prev = min(max(prev + diff, -32768), 32767)
            out.append(prev)
    return np.array(out, dtype=np.int32)


def ping_rtt(k, n=3):
    rt = []
    for i in range(n):
        tag = f"p{time.monotonic_ns()}"
        t0 = k.ws.ping(tag)
        end = t0 + 3
        while tag not in k.ws.pongs and time.monotonic() < end:
            time.sleep(0.005)
        if tag in k.ws.pongs:
            rt.append(round((k.ws.pongs[tag] - t0) * 1e3, 1))
        time.sleep(0.3)
    return rt


def carrier_precise(s, nominal_hz):
    base = nominal_hz - 1500.0
    s.send(f"SET mod=iq low_cut=-5000 high_cut=5000 freq={base / 1e3:.3f}")
    time.sleep(3.0)
    iq = kp.all_iq(s)[-int(s.sample_rate * 2):]
    sp = np.abs(np.fft.fftshift(np.fft.fft(iq * np.hanning(len(iq))))) ** 2
    fa = np.fft.fftshift(np.fft.fftfreq(len(iq), 1 / s.sample_rate))
    return base + float(fa[int(np.argmax(sp))]), base


def retune_series(s, fc, base, n=5, gap=2.5):
    """Retune +-2 kHz; per retune: RTT, flag packet arrival, switch packet arrival, in-packet switch offset."""
    sr = s.sample_rate
    log = []
    cur = base
    for i in range(n):
        rtt = ping_rtt(s, 1)
        nxt = base + (2000.0 if i % 2 == 0 else 0.0)
        npk = len(s.snd)
        t_send = kp.now()
        s.send(f"SET mod=iq low_cut=-5000 high_cut=5000 freq={nxt / 1e3:.3f}")
        time.sleep(gap)
        log.append((t_send, npk, cur, nxt, rtt))
        cur = nxt
    iq = kp.all_iq(s)
    fi = kp.inst_freq(iq, sr, 24)
    out = []
    for t_send, npk, old_lo, new_lo, rtt in log:
        old, new = fc - old_lo, fc - new_lo
        pk = s.snd[npk:]
        if not pk:
            continue
        i0, i1 = pk[0][6], pk[-1][6] + pk[-1][5]
        seg_new = np.abs(fi[i0:i1] - new) < 60
        j = next((k for k in range(len(seg_new) - 200) if seg_new[k:k + 200].all()), None)
        sw = None if j is None else i0 + j
        seg_old = np.nonzero(np.abs(fi[i0:(sw if sw else i1)] - old) < 60)[0]
        last_old = i0 + int(seg_old.max()) if len(seg_old) else None
        # packet containing the switch
        swpk = None
        if last_old is not None:
            swpk = next((p for p in pk if p[6] <= last_old < p[6] + p[5]), None)
        nf = next((p for p in pk if p[1] & 0x04), None)
        out.append({
            "rtt_ms": rtt,
            "switch_pkt_arrival_ms": round((swpk[0] - t_send) * 1e3, 1) if swpk else None,
            "newfreq_pkt_arrival_ms": round((nf[0] - t_send) * 1e3, 1) if nf else None,
            "switch_offset_in_newfreq_pkt": (last_old - nf[6]) if (nf and last_old is not None) else None,
            "switch_in_newfreq_pkt": (nf is not None and swpk is not None and nf[2] == swpk[2]),
            "transition_samples": (sw - last_old) if (sw and last_old) else None})
    return out


def t_r2():
    """Real Kiwi: carrier switch offset, RTT vs retune latency, W/F ADPCM vs plain."""
    w, s = pair(0, 15000.0)
    if w.closed or s.closed:
        kp.summary("R2_failed", {"w": w.closed, "s": s.closed}); return
    zoom_max, cap, bw, cal = geom(w)
    res = {"sample_rate": s.sample_rate, "audio_rate": s.audio_rate, "zoom_cap": cap,
           "rtt_ms_snd": ping_rtt(s, 4), "rtt_ms_wf": ping_rtt(w, 2)}
    best, scan = find_carrier(w, min(cap, 10))
    res["carrier_scan"] = scan
    if not best or best[1] < 25:
        kp.summary("R2", res); s.close(); w.close(); return
    fc, base = carrier_precise(s, best[2])
    res["fc_iq"] = fc
    res["retunes"] = retune_series(s, fc, base, 6, 2.5)
    # ADPCM: plain / compressed / plain at a fixed geometry
    z = min(cap, 9)
    w.send(f"SET zoom={z} cf={fc / 1e3:.3f}")
    time.sleep(2.0)
    phases = {}
    for name, comp in (("plain1", 0), ("comp", 1), ("plain2", 0)):
        w.send(f"SET wf_comp={comp}")
        time.sleep(0.6)
        t0 = kp.now()
        time.sleep(3.5)
        phases[name] = [f for f in w.wf if f[0] >= t0 and f[2] == z]
    plain = [f[5].astype(int) for f in phases["plain1"] + phases["plain2"] if f[3] & 1 == 0]
    comp = [f for f in phases["comp"] if f[3] & 1]
    ad = {"plain_frames": len(plain), "comp_frames": len(comp),
          "comp_bytes": sorted(set(len(f[5]) for f in comp))[:3], "plain_bytes": sorted(set(len(p) for p in plain))[:3]}
    if plain and comp:
        ref = np.median(np.stack(plain), axis=0)
        dec = [ima_decode(f[5].tobytes()) for f in comp]
        for label, sl in (("drop_first_10", slice(10, 1034)), ("drop_last_10", slice(0, 1024))):
            arr = np.median(np.stack([d[sl] for d in dec if len(d) >= 1034]), axis=0)
            A = np.vstack([arr, np.ones_like(arr)]).T
            coef, *_ = np.linalg.lstsq(A, ref, rcond=None)
            fit = A @ coef
            ad[label] = {"scale": round(float(coef[0]), 5), "offset": round(float(coef[1]), 2),
                         "rms_err_after_fit_dB": round(float(np.sqrt(np.mean((fit - ref) ** 2))), 2),
                         "raw_mean_abs_diff": round(float(np.mean(np.abs(arr - ref))), 2)}
        p1 = np.median(np.stack([f[5].astype(int) for f in phases["plain1"]]), axis=0)
        p2 = np.median(np.stack([f[5].astype(int) for f in phases["plain2"]]), axis=0)
        ad["plain_vs_plain_rms_dB"] = round(float(np.sqrt(np.mean((p1 - p2) ** 2))), 2)
    res["adpcm"] = ad
    kp.summary("R2", res)
    s.close()
    w.close()


def t_r3():
    """UberSDR: retune latency without NEW_FREQ."""
    w, s = pair(0, 15000.0)
    if w.closed or s.closed:
        kp.summary("R3_failed", {"w": w.closed, "s": s.closed}); return
    zoom_max, cap, bw, cal = geom(w)
    res = {"sample_rate": s.sample_rate, "packet_samples": sorted(set(p[5] for p in s.snd)),
           "flags": kp.flags_hist(s), "rtt_ms_snd": ping_rtt(s, 4)}
    best, scan = find_carrier(w, min(cap, 10))
    res["carrier_scan"] = scan
    if best and best[1] >= 20:
        fc, base = carrier_precise(s, best[2])
        res["fc_iq"] = fc
        res["retunes"] = retune_series(s, fc, base, 6, 2.5)
        res["flags_after"] = kp.flags_hist(s)
    kp.summary("R3", res)
    s.close()
    w.close()


globals()["t_" + test]()
kp.LOG.close()
os._exit(0)
