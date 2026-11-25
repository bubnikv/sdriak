"""Analysis for ubersession2.py recordings. Usage: python uberanalyze2.py <file.pkl>"""
import gzip, json, os, pickle, struct, sys
import numpy as np

os.environ["UBERSDR_PCMV4_PURE"] = "1"
sys.path.insert(0, os.environ.get("UBERSDR_LIB", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ubersdr_lib")))
from pcm_v4 import PCMv4Decoder  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8")
rec = pickle.load(open(sys.argv[1], "rb"))
ev = rec["events"]
TDF = 162000.0
R = {"mode": rec["mode"], "host": rec["host"], "sockets": rec["sockets"], "closed": rec["closed"],
     "fatal": rec.get("fatal"), "connection": next((e["body"] for e in ev if e["kind"] == "connection"), None)}


def texts(frames):
    out = []
    for t, op, m in frames:
        j = None
        if op == 1:
            try: j = json.loads(m)
            except Exception: pass
        elif op == 2 and m[:2] == b"\x1f\x8b":
            try: j = json.loads(gzip.decompress(m))
            except Exception: pass
        if j is not None:
            out.append((t, j))
    return out


def decode_iq(frames):
    dec = PCMv4Decoder()
    hd, ch, ta, order = [], [], [], []
    for idx, (t, op, m) in enumerate(frames):
        if op != 2 or m[:2] == b"\x1f\x8b":
            continue
        h, s = dec.decode_packet(m)
        hd.append(h); ch.append(np.asarray(s, np.int16)); ta.append(t); order.append(idx)
    if not hd:
        return None
    ns = np.array([h.sample_count // h.channels for h in hd])
    cum = np.concatenate([[0], np.cumsum(ns)[:-1]])
    ts = np.array([h.timestamp_nanos for h in hd], np.float64)
    iq = np.concatenate(ch).astype(np.float32) / 32768.0
    return {"h": hd, "ns": ns, "cum": cum, "ts": ts, "t": np.array(ta), "order": np.array(order),
            "c": iq[0::2] + 1j * iq[1::2], "sr": hd[0].sample_rate}


def iq_summary(d):
    good = d["ts"] > 1e18
    fit = np.polyfit(d["cum"][good], d["ts"][good] - d["ts"][good][0], 1)
    return {"packets": len(d["h"]), "sizes": sorted(set(d["ns"].tolist()))[:5],
            "rate_ts": round(1e9 / fit[0], 3), "arrival_rate": round(float(np.polyfit(d["t"], d["cum"], 1)[0]), 1),
            "baseband_power_med": round(float(np.median([h.baseband_power for h in d["h"]])), 1),
            "noise_med": round(float(np.median([h.noise for h in d["h"]])), 1)}


def carrier_at(d, t_from, secs, near_hz):
    i = int(np.searchsorted(d["t"], t_from)); s0 = int(d["cum"][min(i, len(d["cum"]) - 1)])
    seg = d["c"][s0:s0 + int(d["sr"] * secs)]
    if len(seg) < 1000:
        return None
    sp = np.abs(np.fft.fftshift(np.fft.fft(seg * np.hanning(len(seg))))) ** 2
    fa = np.fft.fftshift(np.fft.fftfreq(len(seg), 1 / d["sr"]))
    sel = np.abs(fa - near_hz) < 400
    k = int(np.argmax(np.where(sel, sp, 0)))
    amp = 2 * np.sqrt(sp[k]) / (0.5 * len(seg))
    return {"f": round(float(fa[k]), 2), "snr": round(float(10 * np.log10(sp[k] / np.median(sp))), 1),
            "dBFS": round(float(20 * np.log10(amp + 1e-30)), 1)}


def find_switch(d, t_cmd, old_off, new_off):
    sr = d["sr"]; i0 = int(np.searchsorted(d["t"], t_cmd))
    if i0 >= len(d["cum"]):
        return None
    s0 = int(d["cum"][i0]); s1 = min(len(d["c"]), s0 + sr * 2)
    mid = 0.5 * (old_off + new_off)
    n = np.arange(s0, s1)
    bb = d["c"][s0:s1] * np.exp(-2j * np.pi * mid * n / sr)
    lp = 129
    k = np.sinc(np.arange(-(lp // 2), lp // 2 + 1) * (2 * max(300.0, abs(new_off - old_off) * 0.75) / sr)) * np.hamming(lp)
    bb = np.convolve(bb, k / k.sum(), mode="same")
    fi = np.convolve(np.angle(bb[1:] * np.conj(bb[:-1])) * sr / (2 * np.pi), np.ones(32) / 32, mode="same")
    tol = max(40.0, abs(new_off - old_off) * 0.3)
    on_new = np.abs(fi - (new_off - mid)) < tol
    j = next((q for q in range(len(on_new) - 400) if on_new[q:q + 400].all()), None)
    if j is None:
        return None
    oo = np.nonzero(np.abs(fi[:j] - (old_off - mid)) < tol)[0]
    lo = int(oo.max()) if len(oo) else j
    sw = s0 + lo
    pk = int(np.searchsorted(d["cum"], sw, side="right") - 1)
    return {"pk": pk, "in_pkt": int(sw - d["cum"][pk]), "pkt_size": int(d["ns"][pk]), "transition": int(j - lo),
            "arrival_ms": round((d["t"][pk] - t_cmd) * 1e3, 1)}


def statuses(frames):
    out = []
    for idx, (t, op, m) in enumerate(frames):
        if op == 1:
            try:
                j = json.loads(m)
                if j.get("type") == "status":
                    out.append((idx, t, j.get("frequency")))
            except Exception:
                pass
    return out


def spec_decode(frames):
    fr, bins, scale = [], None, None
    for t, op, m in frames:
        if op != 2 or m[:4] != b"SPEC":
            continue
        ver, flags = m[4], m[5]
        seq, tsn, cf = struct.unpack("<HQQ", m[6:24])
        f = {"t": t, "seq": seq, "ts": tsn, "cf": cf, "bytes": len(m), "full": flags == 5}
        if flags == 5:
            ref, step = struct.unpack("<hB", m[24:27]); scale = (ref / 100, step / 100)
            bins = scale[0] + np.frombuffer(m[27:], np.uint8).astype(np.float32) * scale[1]
        elif flags == 6 and bins is not None:
            n = len(bins); mb = (n + 7) // 8
            mask = np.unpackbits(np.frombuffer(m[24:24 + mb], np.uint8), bitorder="little")[:n].astype(bool)
            vals = np.frombuffer(m[24 + mb:], np.uint8).astype(np.float32)
            if mask.sum() == len(vals):
                bins = bins.copy(); bins[mask] = scale[0] + vals * scale[1]
            else:
                f["mismatch"] = True
        f["bins"] = None if bins is None else bins.copy()
        fr.append(f)
    return fr


F = rec["frames"]
mode = rec["mode"]

if mode == "vfo":
    A, B = decode_iq(F.get("iqA", [])), decode_iq(F.get("iqB", []))
    R["iqA"] = iq_summary(A) if A else None
    R["iqB"] = iq_summary(B) if B else None
    R["iqA_carrier_TDF"] = carrier_at(A, 3.0, 3.0, 6000.0) if A else None
    R["iqB_carrier_DCF77"] = carrier_at(B, 3.0, 3.0, 6000.0) if B else None
    t_tuneB = next(e["t"] for e in ev if e["kind"] == "tune")
    R["status_after_tuneB"] = {"A": [(round(t - t_tuneB, 3), f) for _, t, f in statuses(F["iqA"]) if t > t_tuneB][:3],
                               "B": [(round(t - t_tuneB, 3), f) for _, t, f in statuses(F["iqB"]) if t > t_tuneB][:3]}
    R["iqB_carrier_after_tune"] = carrier_at(B, t_tuneB + 1.0, 1.5, 4000.0) if B else None
    R["iqA_carrier_after_tuneB"] = carrier_at(A, t_tuneB + 1.0, 1.5, 6000.0) if A else None
    R["iqC"] = {"status": rec.get("iqC_status"), "body": rec.get("iqC_body"),
                "texts": [j for t, j in texts(F.get("iqC", []))][:5], "frames": len(F.get("iqC", []))}
    # back-pressure IQ A
    bp = []
    pauses = [(e["t"], e["secs"]) for e in ev if e["kind"] == "pause_start" and e["ws"] == "iqA"]
    good = A["ts"] > 1e18
    for ps, secs in pauses:
        pe = ps + secs
        after = np.nonzero(A["t"] >= pe)[0]
        burst = int(np.sum((A["t"] >= pe) & (A["t"] < pe + 0.3)))
        # timestamp continuity across the pause: expected vs actual step
        sel = np.nonzero((A["t"] > ps - 1) & (A["t"] < pe + 4))[0]
        dts = np.diff(A["ts"][sel]) - A["ns"][sel][:-1] * (1e9 / A["sr"])
        bp.append({"pause_s": secs, "packets_in_0.3s_after_resume": burst,
                   "max_ts_gap_ms": round(float(np.max(np.abs(dts))) / 1e6, 3) if len(dts) else None,
                   "lag_after_resume_ms": round(float((A["t"][after[0]] - pe) * 1e3), 1) if len(after) else None,
                   "closed": rec["closed"].get("iqA")})
    R["backpressure_iq"] = bp
    # back-pressure spectrum
    S = spec_decode(F.get("spec", []))
    sps = next(((e["t"], e["secs"]) for e in ev if e["kind"] == "pause_start" and e["ws"] == "spec"), None)
    if S and sps:
        ps, secs = sps; pe = ps + secs
        seqs = [(f["t"], f["seq"]) for f in S]
        gaps = [(round(a[0], 2), b[1] - a[1]) for a, b in zip(seqs, seqs[1:]) if (b[1] - a[1]) % 65536 != 1]
        burst = sum(1 for f in S if pe <= f["t"] < pe + 0.3)
        first_full = next((round(f["t"] - pe, 3) for f in S if f["t"] >= pe and f["full"]), None)
        R["backpressure_spec"] = {"seq_gaps": gaps[:6], "frames_in_0.3s_after_resume": burst,
                                  "first_full_after_resume_s": first_full,
                                  "mismatches": sum(1 for f in S if f.get("mismatch"))}

elif mode == "vfo2":
    A, B = decode_iq(F.get("iqA", [])), decode_iq(F.get("iqB", []))
    R["iqA"] = iq_summary(A) if A else None
    R["iqB"] = iq_summary(B) if B else None
    R["handshakes"] = {k: (rec.get(k + "_status"), rec.get(k + "_body")) for k in ("iqB", "iqC", "iqD")}
    R["texts"] = {k: [j if j.get("type") != "status" else {"type": "status", "frequency": j.get("frequency"),
                                                            "mode": j.get("mode"), "sampleRate": j.get("sampleRate")}
                      for t, j in texts(F.get(k, []))][:4] for k in ("iqA", "iqB", "iqC", "iqD")}
    R["closed"] = rec["closed"]
    if B:
        R["iqB_peak"] = carrier_at(B, B["t"][0] + 1.0, 2.0, 0.0)
    if A:
        good = A["ts"] > 1e18
        bp = []
        for ps, secs in [(e["t"], e["secs"]) for e in ev if e["kind"] == "pause_start" and e["ws"] == "iqA"]:
            pe = ps + secs
            after = np.nonzero(A["t"] >= pe)[0]
            sel = np.nonzero((A["t"] > ps - 1) & (A["t"] < pe + 4))[0]
            dts = np.diff(A["ts"][sel]) - A["ns"][sel][:-1] * (1e9 / A["sr"])
            bp.append({"pause_s": secs, "packets_in_0.3s_after_resume": int(np.sum((A["t"] >= pe) & (A["t"] < pe + 0.3))),
                       "max_ts_gap_ms": round(float(np.max(np.abs(dts))) / 1e6, 3) if len(dts) else None,
                       "first_after_resume_ms": round(float((A["t"][after[0]] - pe) * 1e3), 1) if len(after) else None,
                       "catch_up_s": round(float(A["t"][after][np.argmax((A["t"][after][1:] - A["t"][after][:-1]) > 0.004)] - pe), 3)
                       if len(after) > 2 else None})
        R["backpressure_iq"] = bp
        # latency after the pauses: arrival minus timestamp, before vs after
        lat = A["t"] - A["ts"] / 1e9
        first = [e["t"] for e in ev if e["kind"] == "pause_start" and e["ws"] == "iqA"][0]
        R["latency_shift_after_pauses_ms"] = round(float((np.median(lat[A["t"] > A["t"][-1] - 3]) -
                                                          np.median(lat[(A["t"] > 4) & (A["t"] < first)])) * 1e3), 1)
    if B:
        R["iqB_after_primary_pauses"] = {"packets": len(B["h"]), "last_arrival": round(float(B["t"][-1]), 2)}

elif mode == "final":
    def retune_table(d, frames, tunes, f0):
        st = statuses(frames); out = []; prev = f0
        for e in tunes:
            sw = find_switch(d, e["t"], TDF - prev, TDF - e["f"])
            echo = next(((idx, t, f) for idx, t, f in st if t > e["t"] and f == int(e["f"])), None)
            r = {"to": e["f"], "echo_ms": round((echo[1] - e["t"]) * 1e3, 1) if echo else None}
            if sw:
                r.update(sw)
                if echo:
                    r["pkts_echo_to_switch"] = sw["pk"] - int(np.searchsorted(d["order"], echo[0]))
            out.append(r); prev = e["f"]
        return out
    A = decode_iq(F.get("iqA", []))
    R["allowed_iq_modes"] = rec.get("allowed_iq_modes")
    R["iqA"] = iq_summary(A) if A else None
    tA = [e for e in ev if e["kind"] == "tune" and e["ws"] == "iqA"]
    tB = [e for e in ev if e["kind"] == "tune" and e["ws"] == "iqB"]
    if A:
        R["primary_retunes"] = retune_table(A, F["iqA"], tA, TDF - 6000)
    B = decode_iq(F.get("iqB", []))
    if B:
        R["iqB"] = iq_summary(B)
        R["iqB_carrier"] = carrier_at(B, B["t"][0] + 0.5, 1.5, 1500.0)
        R["secondary_retunes"] = retune_table(B, F["iqB"], tB, TDF - 1500)
        R["iqB_texts"] = [j for t, j in texts(F["iqB"]) if j.get("type") != "status"][:3]
    wide = {}
    for m in ("iq96", "iq192"):
        if m in F:
            d = decode_iq(F[m])
            if d:
                arr = d["t"]
                wide[m] = dict(iq_summary(d), bytes_per_s=round(sum(len(x[2]) for x in F[m] if x[1] == 2) / (arr[-1] - arr[0]), 0),
                               carrier=carrier_at(d, arr[0] + 1.0, 2.0, 6000.0),
                               profiles=sorted(set(h.profile for h in d["h"])), shifts=sorted(set(h.shift for h in d["h"]))[:4])
            else:
                wide[m] = {"frames": len(F[m]), "texts": [j for t, j in texts(F[m])][:3]}
    R["wide"] = wide

elif mode == "ret":
    A = decode_iq(F.get("iqA", []))
    R["iqA"] = iq_summary(A) if A else None
    st = statuses(F["iqA"])
    tunes = [e for e in ev if e["kind"] == "tune"]
    burst = next(e["t"] for e in ev if e["kind"] == "burst_start")
    burst_end = next(e["t"] for e in ev if e["kind"] == "burst_end")
    singles = [e for e in tunes if e["t"] < burst]
    out = []
    prev = TDF - 6000
    for e in singles:
        old_off, new_off = TDF - prev, TDF - e["f"]
        sw = find_switch(A, e["t"], old_off, new_off)
        echo = next(((idx, t, f) for idx, t, f in st if t > e["t"] and f == int(e["f"])), None)
        r = {"to": e["f"], "echo_ms": round((echo[1] - e["t"]) * 1e3, 1) if echo else None}
        if sw:
            r.update(sw)
            if echo:
                pk_before_echo = int(np.searchsorted(A["order"], echo[0]))
                r["pkts_echo_to_switch"] = sw["pk"] - pk_before_echo
        out.append(r)
        prev = e["f"]
    R["single_retunes"] = out
    bt = [e for e in tunes if burst <= e["t"] <= burst_end + 0.01]
    echoed = [(round(t - burst, 3), f) for idx, t, f in st if burst <= t <= burst_end + 3]
    R["burst"] = {"commands": len(bt), "echoes": len(echoed), "echo_freqs": [f for _, f in echoed],
                  "last_cmd": bt[-1]["f"] if bt else None,
                  "carrier_after_burst": carrier_at(A, burst_end + 1.5, 1.0, TDF - bt[-1]["f"]) if bt else None}
    far = [e for e in tunes if e["t"] > burst_end + 0.5]
    R["far_jumps"] = [{"to": e["f"], "echo_ms": next((round((t - e["t"]) * 1e3, 1) for idx, t, f in st
                                                      if t > e["t"] and f == int(e["f"])), None)} for e in far]
    R["carrier_after_return"] = carrier_at(A, far[-1]["t"] + 1.0, 1.0, 6000.0) if far else None
    R["reconnect"] = {"status": rec.get("reconnect_status"), "body": rec.get("reconnect_body"),
                      "frames": len(F.get("iqR", [])), "texts": [j for t, j in texts(F.get("iqR", []))][:3]}

elif mode in ("lvl", "lvl2"):
    A = decode_iq(F.get("iqA", []))
    R["iqA"] = iq_summary(A) if A else None
    R["iq_status"] = next((j for t, j in texts(F["iqA"]) if j.get("type") == "status"), None)
    R["iq_carrier"] = carrier_at(A, 3.0, 3.0, 6000.0) if A else None
    S = spec_decode(F.get("spec", []))
    cfg = [(t, j) for t, j in texts(F.get("spec", [])) if j.get("type") == "config"]
    views = []
    for e in [x for x in ev if x["kind"] == "view"]:
        c = next(((round(t - e["t"], 3), j) for t, j in cfg if t > e["t"]), None)
        fr = [f for f in S if e["t"] + 1.5 < f["t"] < e["t"] + 3.4 and f["bins"] is not None]
        v = {"view": e["label"], "req_f": e["f"], "req_bw": e["bw"]}
        if c:
            v.update(cfg_ms=round(c[0] * 1e3, 1), cfg_centre=c[1].get("centerFreq"), cfg_bins=c[1].get("binCount"),
                     cfg_bw=c[1].get("binBandwidth"), cfg_span=c[1].get("totalBandwidth"))
        if fr:
            last = fr[-1]
            v["frame_centre"] = last["cf"]; v["frame_bins"] = len(last["bins"])
            med = np.median(np.stack([f["bins"] for f in fr if len(f["bins"]) == len(last["bins"])]), axis=0)
            if c and c[1].get("binBandwidth"):
                bw = c[1]["binBandwidth"]; n = len(med)
                f0 = last["cf"] - n * bw / 2
                kexp = (TDF - f0) / bw
                if 0 <= kexp < n:
                    win = slice(max(0, int(kexp) - 3), min(n, int(kexp) + 4))
                    kp = int(np.argmax(med[win])) + win.start
                    v.update(tdf_peak_dB=round(float(med[kp]), 2), tdf_peak_bin=kp, tdf_expected_bin=round(kexp, 2),
                             floor_dB=round(float(np.median(med)), 1))
            v["fps"] = round(len(fr) / 1.9, 1)
        views.append(v)
    R["views"] = views
    if A and S:
        # header power per packet around the views, for comparison
        R["header_power_med"] = round(float(np.median([h.baseband_power for h in A["h"]])), 1)
    rates = []
    for e in [x for x in ev if x["kind"] == "set_rate"]:
        fr = [f for f in S if e["t"] + 1.0 < f["t"] < e["t"] + 5.0]
        rates.append({"divisor": e["divisor"], "fps": round(len(fr) / 4.0, 2)})
    R["set_rate"] = rates
print(json.dumps(R, default=str, ensure_ascii=False, indent=1)[:9000])
