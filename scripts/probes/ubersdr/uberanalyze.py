"""Offline analysis of one ubersession.py recording. Usage: python uberanalyze.py <file.pkl>"""
import json, os, pickle, struct, sys, time
import numpy as np

os.environ["UBERSDR_PCMV4_PURE"] = "1"
sys.path.insert(0, os.environ.get("UBERSDR_LIB", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ubersdr_lib")))
from pcm_v4 import PCMv4Decoder  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8")
rec = pickle.load(open(sys.argv[1], "rb"))
R = {"host": rec["host"], "connection": rec.get("connection"), "rtt_ms": rec.get("rtt_ms"),
     "iq_handshake": rec.get("iq_handshake"), "spec_handshake": rec.get("spec_handshake"),
     "iq_closed": rec.get("iq_closed"), "spec_closed": rec.get("spec_closed"), "fatal": rec.get("fatal")}
d = rec.get("description")
if isinstance(d, dict):
    R["description_keys"] = sorted(d.keys())[:40]
ev = rec["events"]


def jtexts(frames):
    out = []
    for t, op, m in frames:
        if op == 1:
            try:
                out.append((t, json.loads(m)))
            except Exception:
                pass
    return out


# ------------------------------------------------------------------ IQ
iqf = [(t, m) for t, op, m in rec.get("iq_frames", []) if op == 2]
txt = jtexts(rec.get("iq_frames", []))
R["iq_text_types"] = {}
for t, j in txt:
    R["iq_text_types"][j.get("type")] = R["iq_text_types"].get(j.get("type"), 0) + 1
R["iq_status_example"] = next((j for t, j in txt if j.get("type") == "status"), None)
dec = PCMv4Decoder()
hdrs, chunks, t_arr = [], [], []
t0 = time.time()
for t, m in iqf:
    h, s = dec.decode_packet(m)
    hdrs.append(h)
    chunks.append(np.asarray(s, dtype=np.int16))
    t_arr.append(t)
R["iq_decode_s"] = round(time.time() - t0, 1)
if hdrs:
    sr = hdrs[0].sample_rate
    nsamp = [h.sample_count // h.channels for h in hdrs]
    ts = np.array([h.timestamp_nanos for h in hdrs], dtype=np.float64)
    cum = np.concatenate([[0], np.cumsum(nsamp)[:-1]])
    good = ts > 1e18
    R["iq_unstamped_packets"] = int((~good).sum())
    fit = np.polyfit(cum[good], ts[good] - ts[good][0], 1)      # ns per sample
    resid = (ts[good] - ts[good][0]) - np.polyval(fit, cum[good])
    steps = np.diff(ts[good]) - np.diff(cum[good]) * fit[0]
    arr = np.array(t_arr)
    R["iq"] = {"packets": len(hdrs), "sample_rate": sr, "channels": hdrs[0].channels,
               "samples_per_packet": sorted(set(nsamp))[:6], "profiles": sorted(set(h.profile for h in hdrs)),
               "shift_values": sorted(set(h.shift for h in hdrs))[:6],
               "escape_pkts": sum(h.escape for h in hdrs), "silent_pkts": sum(h.silent for h in hdrs),
               "rate_from_timestamps": round(1e9 / fit[0], 4),
               "timestamp_resid_ns_rms": round(float(np.sqrt(np.mean(resid ** 2))), 1),
               "timestamp_resid_ns_max": round(float(np.max(np.abs(resid))), 1),
               "arrival_rate": round(float(np.polyfit(arr, cum, 1)[0]), 2),
               "timestamp_step_jumps_over_1ms": int((np.abs(steps) > 1e6).sum()),
               "baseband_power_dBFS_median": round(float(np.median([h.baseband_power for h in hdrs])), 1),
               "noise_median": round(float(np.median([h.noise for h in hdrs])), 1),
               "bytes_per_s": round(sum(len(m) for _, m in iqf) / (arr[-1] - arr[0]), 0)}
    iq = np.concatenate(chunks).astype(np.float32) / 32768.0
    c = iq[0::2] + 1j * iq[1::2]
    # carrier: strongest line in the first 4 s
    seg = c[: sr * 4]
    sp = np.abs(np.fft.fftshift(np.fft.fft(seg * np.hanning(len(seg))))) ** 2
    fa = np.fft.fftshift(np.fft.fftfreq(len(seg), 1 / sr))
    near = np.abs(fa - 6000.0) < 300                            # TDF expected at +6 kHz
    k = int(np.argmax(np.where(near, sp, 0)))
    f_off = float(fa[k])
    snr = 10 * np.log10(sp[k] / np.median(sp))
    R["carrier"] = {"offset_hz": round(f_off, 2), "snr_dB": round(float(snr), 1)}
    # carrier level in dBFS (amplitude of the line, Hann coherent gain 0.5)
    amp = 2 * np.sqrt(sp[k]) / (0.5 * len(seg))
    R["carrier"]["level_dBFS"] = round(float(20 * np.log10(amp)), 1)
    # retunes: isolate the carrier (mix to DC, low-pass), then find the switch sample
    tunes = [e for e in ev if e["kind"] == "tune"]
    centre0 = rec["carrier_hint"] - 6000.0
    out = []
    pkt_start = cum
    # stream order: index of each binary packet and each status text in the raw frame list
    order_bin, order_status = [], []
    for idx, (t, op, m) in enumerate(rec["iq_frames"]):
        if op == 2:
            order_bin.append(idx)
        elif op == 1:
            try:
                if json.loads(m).get("type") == "status":
                    order_status.append((idx, t))
            except Exception:
                pass
    order_bin = np.array(order_bin)
    lp = 129
    kern = np.sinc(np.arange(-(lp // 2), lp // 2 + 1) * (2 * 1500.0 / sr)) * np.hamming(lp)
    kern /= kern.sum()
    n = np.arange(len(c))
    for i, e in enumerate(tunes):
        prev_f = tunes[i - 1]["f"] if i else centre0
        exp_new = f_off + (centre0 - e["f"])
        exp_old = f_off + (centre0 - prev_f)
        i0 = int(np.searchsorted(arr, e["t"]))
        if i0 >= len(pkt_start):
            continue
        s0 = int(pkt_start[i0])
        s1 = min(len(c), s0 + sr * 2)
        mid = 0.5 * (exp_new + exp_old)
        bb = c[s0:s1] * np.exp(-2j * np.pi * mid * n[s0:s1] / sr)
        bb = np.convolve(bb, kern, mode="same")
        fi = np.convolve(np.angle(bb[1:] * np.conj(bb[:-1])) * sr / (2 * np.pi), np.ones(32) / 32, mode="same")
        want_new, want_old = exp_new - mid, exp_old - mid
        on_new = np.abs(fi - want_new) < 150
        j = next((q for q in range(len(on_new) - 400) if on_new[q:q + 400].all()), None)
        if j is None:
            out.append({"tune_t": e["t"], "found": False}); continue
        on_old = np.nonzero(np.abs(fi[:j] - want_old) < 150)[0]
        last_old = int(on_old.max()) if len(on_old) else None
        sw = s0 + (last_old if last_old is not None else j)
        pk = int(np.searchsorted(pkt_start, sw, side="right") - 1)
        st_after = next(((idx, t) for idx, t in order_status if t > e["t"]), None)
        pkts_between = None
        if st_after:
            # binary packets received before the status message, counted from the switch packet
            pkts_before_status = int(np.searchsorted(order_bin, st_after[0]))
            pkts_between = pkts_before_status - pk
        out.append({"tune_to_switch_arrival_ms": round((arr[pk] - e["t"]) * 1e3, 1),
                    "switch_sample_in_packet": int(sw - pkt_start[pk]), "packet_samples": int(nsamp[pk]),
                    "transition_samples": int(j - last_old) if last_old is not None else None,
                    "switch_packet_timestamp_ns": int(ts[pk]),
                    "status_after_ms": round((st_after[1] - e["t"]) * 1e3, 1) if st_after else None,
                    "packets_switch_to_status": pkts_between})
    R["retunes"] = out
    # spectrum-socket RTT from (gzip) pongs vs ping sends
    sp_pings = [e["t"] for e in ev if e["kind"] == "send" and e.get("ws") == "spec" and '"ping"' in e.get("msg", "")]
    R["spec_ping_send_t"] = sp_pings
    R["iq_timestamps_first_last_ns"] = [int(ts[0]), int(ts[-1])]

# ------------------------------------------------------------------ spectrum v2
spf = [(t, m) for t, op, m in rec.get("spec_frames", []) if op == 2]
stxt = jtexts(rec.get("spec_frames", []))
R["spec_text"] = [(t, {k: j.get(k) for k in ("type", "centerFreq", "binCount", "binBandwidth", "totalBandwidth",
                                               "error", "status", "frequency")}) for t, j in stxt][:40]
frames, bins, scale = [], None, None
stxt_gz = []
import gzip
for t, m in spf:
    if m[:2] == b"\x1f\x8b":
        try:
            stxt_gz.append((t, json.loads(gzip.decompress(m))))
        except Exception:
            pass
        continue
    if m[:4] != b"SPEC":
        frames.append({"t": t, "bad_magic": m[:8].hex(), "len": len(m)}); continue
    ver, flags = m[4], m[5]
    seq, tsn, cf = struct.unpack("<HQQ", m[6:24])
    info = {"t": t, "ver": ver, "flags": flags, "seq": seq, "ts": tsn, "cf": cf, "bytes": len(m)}
    if ver == 2 and flags == 0x05:
        ref, step = struct.unpack("<hB", m[24:27])
        codes = np.frombuffer(m[27:], dtype=np.uint8).astype(np.float32)
        scale = (ref / 100.0, step / 100.0)
        bins = scale[0] + codes * scale[1]
        info.update(n=len(codes), full=True, ref=scale[0], step=scale[1])
    elif ver == 2 and flags == 0x06 and bins is not None:
        n = len(bins)
        mb = (n + 7) // 8
        mask = np.unpackbits(np.frombuffer(m[24:24 + mb], dtype=np.uint8), bitorder="little")[:n].astype(bool)
        vals = np.frombuffer(m[24 + mb:], dtype=np.uint8).astype(np.float32)
        if mask.sum() == len(vals):
            bins = bins.copy(); bins[mask] = scale[0] + vals * scale[1]
            info.update(n=n, full=False, changed=int(mask.sum()))
        else:
            info.update(delta_mismatch=(int(mask.sum()), len(vals)))
    info["peak_dB"] = float(np.max(bins)) if bins is not None else None
    info["peak_idx"] = int(np.argmax(bins)) if bins is not None else None
    info["median_dB"] = float(np.median(bins)) if bins is not None else None
    frames.append(info)
stxt = sorted(stxt + stxt_gz, key=lambda x: x[0])
R["spec_text"] = [(t, {k: j.get(k) for k in ("type", "centerFreq", "binCount", "binBandwidth", "totalBandwidth",
                                               "error", "status", "frequency")}) for t, j in stxt][:40]
bad = [f for f in frames if "bad_magic" in f]
R["spec_bad_magic"] = {"count": len(bad), "examples": bad[:3]}
_pings = [e["t"] for e in ev if e["kind"] == "send" and e.get("ws") == "spec" and '"ping"' in e.get("msg", "")]
_pongs = [t for t, j in stxt if j.get("type") == "pong"]
R["spec_rtt_ms"] = [round((min([p for p in _pongs if p > s], default=s) - s) * 1e3, 1) for s in _pings]
R["spec_text_gzip_count"] = len(stxt_gz)
frames = [f for f in frames if "bad_magic" not in f]
if frames:
    tt = np.array([f["t"] for f in frames])
    seqs = [f["seq"] for f in frames if "seq" in f]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if (b - a) % 65536 != 1)
    fulls = [f for f in frames if f.get("full")]
    R["spec"] = {"frames": len(frames), "fps": round(len(frames) / (tt[-1] - tt[0]), 2),
                 "full_frames": len(fulls), "bin_counts": sorted(set(f.get("n") for f in frames if f.get("n"))),
                 "bytes_full_median": int(np.median([f["bytes"] for f in fulls])) if fulls else None,
                 "bytes_delta_median": int(np.median([f["bytes"] for f in frames if f.get("full") is False])) if any(
                     f.get("full") is False for f in frames) else None,
                 "bytes_per_s": round(sum(f["bytes"] for f in frames) / (tt[-1] - tt[0]), 0),
                 "seq_gaps": gaps, "steps_dB": sorted(set(f.get("step") for f in fulls))[:5],
                 "delta_mismatches": sum(1 for f in frames if "delta_mismatch" in f)}
    # view changes: first frame with the new centre after each view command
    vc = []
    for e in ev:
        if e["kind"] == "view" or e["kind"] in ("burst_start", "reset"):
            nxt = [f for f in frames if f["t"] > e["t"]]
            want = e.get("f")
            first_new = next((f for f in nxt if want is None or abs(f["cf"] - want) < 1), None)
            st = next(((round((t - e["t"]) * 1e3, 1), j.get("centerFreq"), j.get("binCount"), j.get("binBandwidth"))
                       for t, j in stxt if t > e["t"] and j.get("type") in ("status", "config")), None)
            errs = [j for t, j in stxt if e["t"] < t < e["t"] + 3 and j.get("type") == "error"]
            vc.append({"cmd": e.get("label", e["kind"]), "first_frame_new_centre_ms":
                       round((first_new["t"] - e["t"]) * 1e3, 1) if first_new else None,
                       "status_reply": st, "errors": [x.get("error") for x in errs][:3],
                       "frame_peak_dB": first_new.get("peak_dB") if first_new else None})
    R["spec_views"] = vc
    # timestamp alignment spectrum vs IQ, and spectrum ts vs arrival
    ts_s = np.array([f["ts"] for f in frames if "ts" in f], dtype=np.float64)
    tarr = np.array([f["t"] for f in frames if "ts" in f])
    off = (tarr - tarr[0]) - (ts_s - ts_s[0]) / 1e9
    R["spec_ts_vs_arrival_jitter_ms"] = round(float(np.std(off) * 1e3), 1)
    if hdrs:
        # same mapping for IQ: arrival - ts; compare constant parts
        ia = np.array(t_arr)[good]; its = ts[good] / 1e9
        R["arrival_minus_ts_s"] = {"iq_median": round(float(np.median(ia - its)), 3),
                                   "spec_median": round(float(np.median(tarr - ts_s / 1e9)), 3),
                                   "note": "arrival is local monotonic, ts is server epoch; only the difference between the two medians matters"}
print(json.dumps(R, default=str, ensure_ascii=False, indent=1)[:12000])
