"""Offline analysis of an owrxsession.py recording.

Usage: python owrxanalyze.py <rec.pkl>

Reports: the FFT stream (size, rate, dB range), the IQ stream (rate from
sample count against arrival time, level), the IQ spectrum's two halves
under the default passband and under +-24 kHz cuts, and for each offset
retune the arrival time of the first IQ block whose spectrum matches the
new offset (block spectra compared with templates taken well before and
well after the command)."""
import json, pickle, sys
import numpy as np

sys.stdout.reconfigure(encoding="utf-8")
rec = pickle.load(open(sys.argv[1], "rb"))
frames = rec["frames"]
sent = [(e["t"], e["msg"]) for e in rec["events"] if e["kind"] == "sent"]

cfg = {}
for t, k, m in frames:
    if k == "text" and m.startswith("{"):
        j = json.loads(m)
        if j.get("type") == "config":
            cfg.update(j["value"])
print("config:", {x: cfg.get(x) for x in ("samp_rate", "center_freq", "fft_size", "fft_compression",
                                            "audio_compression", "profile_id", "start_mod")})

from owrxcommon import adpcm_plain, AdpcmStereoSync

# ---- FFT ----
fft = [(t, m) for t, k, m in frames if k == 1]
if fft:
    ts = np.array([t for t, _ in fft])
    print(f"FFT: {len(fft)} frames, {len(fft) / (ts[-1] - ts[0]):.2f}/s, payload {len(fft[0][1])} B")
    if cfg.get("fft_compression") == "adpcm":
        rows = [adpcm_plain(m)[10:] / 100.0 for _, m in fft[::20]]
    else:
        rows = [np.frombuffer(m, "<f4") for _, m in fft[::20]]
    rows = np.array([r[: cfg.get("fft_size", len(r))] for r in rows])
    print(f"FFT bins {rows.shape[1]}, dB median {np.median(rows):.1f}, p1 {np.percentile(rows, 1):.1f}, "
          f"p99.9 {np.percentile(rows, 99.9):.1f}")

# ---- IQ (type 6, uncompressed int16 stereo = I, Q) ----
iq_frames = [(t, m) for t, k, m in frames if k == 6]
adpcm = AdpcmStereoSync() if cfg.get("audio_compression") == "adpcm" else None
print("IQ coding:", "ADPCM (4-bit)" if adpcm else "int16")
starts, chunks, total = [], [], 0
for t, m in iq_frames:
    if adpcm:
        ci, cq = adpcm.decode(m)
        z = (ci.astype(np.float32) + 1j * cq.astype(np.float32)) / 32768.0
    else:
        a = np.frombuffer(m, "<i2").astype(np.float32) / 32768.0
        z = a[0::2] + 1j * a[1::2]
    starts.append((t, total))
    chunks.append(z)
    total += len(z)
iq = np.concatenate(chunks)
t_first, t_last = starts[0][0], starts[-1][0]
n_after_first = total - len(chunks[0])
rate = (starts[-1][1] - starts[0][1]) / (t_last - t_first)
print(f"IQ: {len(iq_frames)} messages, {total} samples, ~{rate:.0f} S/s by arrival, "
      f"{len(chunks[1])} samples/message, rms {np.sqrt(np.mean(np.abs(iq) ** 2)):.3f}, "
      f"peak |I|,|Q| {np.max(np.abs(iq.real)):.3f} {np.max(np.abs(iq.imag)):.3f}")
FS = 48000.0


def index_at(t):
    """Stream index of the first sample arriving after time t."""
    for ta, idx in starts:
        if ta >= t:
            return idx
    return total


N = 2048
win = np.hanning(N)


def spec(a, b):
    seg = iq[a:b]
    k = len(seg) // N
    if k == 0:
        return None
    x = seg[: k * N].reshape(k, N) * win
    return np.mean(np.abs(np.fft.fftshift(np.fft.fft(x, axis=1), axes=1)) ** 2, axis=0)


def halves(p):
    f = (np.arange(N) - N / 2) * FS / N
    neg = p[(f < -2000) & (f > -22000)].mean()
    pos = p[(f > 2000) & (f < 22000)].mean()
    return 10 * np.log10(neg), 10 * np.log10(pos)


t_cut = next(t for t, m in sent if "low_cut" in m)
t_ret = [(t, json.loads(m)["params"]["offset_freq"]) for t, m in sent if "offset_freq" in m and "mod" not in m]
a, b = index_at(t_first + 1.0), index_at(t_cut)
n, p = halves(spec(a, b))
print(f"default passband: negative half {n:.1f} dB, positive half {p:.1f} dB")
a, b = index_at(t_cut + 1.0), index_at(t_ret[0][0])
n, p = halves(spec(a, b))
print(f"+-24 kHz cuts:    negative half {n:.1f} dB, positive half {p:.1f} dB")

# Passband shape under the cuts, in 2 kHz steps.
P = spec(index_at(t_cut + 1.0), index_at(t_ret[0][0]))
f = (np.arange(N) - N / 2) * FS / N
print("shape (dB rel. median):", " ".join(
    f"{int(lo / 1000):+d}k:{10 * np.log10(P[(f >= lo) & (f < lo + 2000)].mean() / np.median(P)):+.0f}"
    for lo in range(-24000, 24000, 4000)))

# Retunes: per-block log spectra against before/after templates.
B = 1024
wb = np.hanning(B)


def blocks(a, b):
    out = []
    for s in range(a, b - B, B):
        out.append((s, 10 * np.log10(np.abs(np.fft.fftshift(np.fft.fft(iq[s:s + B] * wb))) ** 2 + 1e-12)))
    return out


for i, (tc, off) in enumerate(t_ret):
    t_next = t_ret[i + 1][0] if i + 1 < len(t_ret) else t_last
    pre = blocks(index_at(tc - 2.0), index_at(tc - 0.2))
    post = blocks(index_at(tc + 2.0), index_at(t_next - 0.2))
    if not pre or not post:
        continue
    tp = np.mean([s for _, s in pre], axis=0)
    tq = np.mean([s for _, s in post], axis=0)
    sep = np.mean(np.abs(tp - tq))
    switch = None
    for s, sp in blocks(index_at(tc - 0.2), index_at(tc + 2.0)):
        if np.mean(np.abs(sp - tq)) < np.mean(np.abs(sp - tp)):
            switch = s
            break
    if switch is None:
        print(f"retune to {off:+d}: no switch found (template separation {sep:.2f} dB)")
        continue
    arr = next(ta for ta, idx in reversed(starts) if idx <= switch)
    print(f"retune to {off:+d}: template separation {sep:.2f} dB; switch near sample {switch}, "
          f"in a message arriving {1000 * (arr - tc):.0f} ms after the command")

# Carrier tracking (when the session tuned onto one): the strongest IQ bin
# per block, against the offset it should sit at after each command.
c0 = rec.get("carrier_offset", 0)
if c0:
    # Where the carrier rests in the IQ before the first retune (the FFT's
    # bin width leaves the tune a few hundred hertz off).
    rest = blocks(index_at(t_ret[0][0] - 3.0), index_at(t_ret[0][0] - 0.2))
    peaks = [(int(np.argmax(sp)) - B / 2) * FS / B for _, sp in rest]
    base = float(np.median(peaks))
    print(f"carrier at {c0:+d} Hz from the centre; rests at {base:+.0f} Hz in the IQ")
    for i, (tc, off) in enumerate(t_ret):
        want = base - (off - c0)
        a = index_at(tc - 0.5)
        found = None
        for s0, sp in blocks(a, index_at(tc + 3.0)):
            k = int(np.argmax(sp))
            fk = (k - B / 2) * FS / B
            if abs(fk - want) < 2 * FS / B and sp[k] - np.median(sp) > 15:
                found = s0
                break
        if found is None:
            print(f"  command {off - c0:+d}: carrier never reached {want:+.0f} Hz")
            continue
        arr = next(ta for ta, idx in reversed(starts) if idx <= found)
        print(f"  command {off - c0:+d} (carrier to {want:+.0f} Hz): first block at the new place is sample {found}, "
              f"in a message arriving {1000 * (arr - tc):.0f} ms after the command")

