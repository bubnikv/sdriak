"""Offline analysis of a spyprobe.py "main" recording.

  python spyanalyze.py <outdir>/main_<host>_<port>

Prints the message timeline (settings, CLIENT_SYNC, PONG with their IQ
sample index), the carrier frequency measured from the IQ, where each
retune's switch lands relative to the command, the sync and the pong, and
the FFT bin geometry under both span hypotheses.
"""
import json
import sys

import numpy as np


def load(prefix):
    with open(prefix + ".json") as f:
        meta = json.load(f)
    data = np.load(prefix + ".npz")
    return meta, data


def carrier_in_iq(iq, rate, lo, hi):
    """Strongest tone between lo and hi Hz (baseband), parabolic interpolation."""
    n = 1 << int(np.floor(np.log2(len(iq))))
    x = iq[:n] * np.hanning(n)
    spec = np.abs(np.fft.fftshift(np.fft.fft(x))) ** 2
    freqs = (np.arange(n) - n / 2) * rate / n
    mask = (freqs > lo) & (freqs < hi)
    k = np.flatnonzero(mask)[np.argmax(spec[mask])]
    a, b, c = np.log(spec[k - 1:k + 2] + 1e-30)
    delta = 0.5 * (a - c) / (a - 2 * b + c)
    return (k - n / 2 + delta) * rate / n, 10 * np.log10(spec[k] / np.median(spec[mask]))


def inst_freq(iq, rate, shift, smooth):
    """Instantaneous frequency of iq mixed by -shift, smoothed over `smooth` samples."""
    t = np.arange(len(iq)) / rate
    y = iq * np.exp(-2j * np.pi * shift * t)
    kernel = np.ones(smooth) / smooth
    y = np.convolve(y, kernel, mode="same")
    d = y[1:] * np.conj(y[:-1])
    d = np.convolve(d, kernel, mode="same")
    return np.angle(d) * rate / (2 * np.pi)


def main(prefix):
    meta, data = load(prefix)
    s = meta["summary"]
    ev = meta["events"]
    iq = data["iq"].astype(np.complex64)
    rate = s["iq_rate"]
    print(f"{s['host']}:{s['port']} device {s['device']['DeviceType']} max SR {s['device']['MaximumSampleRate']} "
          f"max BW {s['device']['MaximumBandwidth']} IQ rate {rate}")
    print(f"IQ samples {len(iq)}, fft frames {s['fft_frames']}, seq gaps {s['seq_gaps']}, syncs {s['syncs']}")

    print("\n-- timeline (t, iq index, event)")
    for t, kind, kw in ev:
        if kind in ("set", "client_sync", "pong", "ping", "closed"):
            extra = {k: v for k, v in kw.items() if k != "iq"}
            if kind == "client_sync":
                extra = {k: extra[k] for k in ("DeviceCenterFrequency", "IQCenterFrequency", "FFTCenterFrequency",
                                               "MinimumIQCenterFrequency", "MaximumIQCenterFrequency", "CanControl")}
            print(f"{t:8.3f} {kw['iq']:9d} {kind:12s} {extra}")

    # The retune block: IQ_FREQUENCY commands after the first one sent with the carrier known.
    base, step, offset = s["base"], s["step"], s["offset"]
    sets = [(t, kw["iq"], kw["value"]) for t, kind, kw in ev if kind == "set" and kw["setting"] == "IQ_FREQUENCY"]
    first = next(i for i, x in enumerate(sets) if x[2] == base)
    retunes = sets[first:first + 7]
    # Carrier from the IQ while tuned to base (between the first and second retune command).
    a, b = retunes[0][1], retunes[1][1]
    seg = iq[a + (b - a) // 3:b]
    f_off, snr = carrier_in_iq(seg, rate, 0.25 * offset, 1.75 * offset + step)
    fc = base + f_off
    print(f"\ncarrier from IQ: {fc:.1f} Hz (at {f_off:+.1f} Hz from IQ centre {base}), {snr:.0f} dB over the band median")

    def after(i0, kind):
        for t, k, kw in ev:
            if k == kind and kw["iq"] >= i0:
                return kw["iq"], t
        return None, None

    print("\n-- retunes: switch sample vs command, sync, pong (IQ samples; ms at the IQ rate)")
    smooth = max(8, int(rate / (4 * step)))
    for (t_cmd, i_cmd, val), prev in zip(retunes[1:], retunes[:-1]):
        old_off = fc - prev[2]
        new_off = fc - val
        mid = (old_off + new_off) / 2
        lo = i_cmd
        hi = min(len(iq), i_cmd + int(1.2 * rate))
        f = inst_freq(iq[lo:hi], rate, mid, smooth)
        sign_old = np.sign(old_off - mid)
        # first sample after which the frequency stays on the new side for 5 ms
        hold = int(0.005 * rate)
        new_side = np.sign(f) != sign_old
        sw = None
        for k in range(len(new_side) - hold):
            if new_side[k] and new_side[k:k + hold].all():
                sw = k
                break
        i_sync, _ = after(i_cmd, "client_sync")
        i_pong, _ = after(i_cmd, "pong")
        if sw is None:
            print(f"  cmd {val}: no switch found")
            continue
        i_sw = lo + sw
        ms = lambda n: 1000.0 * n / rate
        print(f"  cmd {val} at {i_cmd}: switch at +{i_sw - i_cmd} ({ms(i_sw - i_cmd):.1f} ms); "
              f"sync {'-' if i_sync is None else f'+{i_sync - i_cmd} (switch {i_sw - i_sync:+d})'}; "
              f"pong {'-' if i_pong is None else f'+{i_pong - i_cmd} (switch {i_sw - i_pong:+d})'}")

    print("\n-- FFT geometry: carrier bin under span = MaximumBandwidth/2^n (BW) or MaximumSampleRate/2^n (SR)")
    dev = s["device"]
    for stage, r in sorted(s["stages"].items(), key=lambda x: int(x[0])):
        n = r["pixels"]
        out = []
        for name, full in (("BW", dev["MaximumBandwidth"]), ("SR", dev["MaximumSampleRate"])):
            span = full / (1 << int(stage))
            pos = (fc - (r["fft_centre"] - span / 2)) / (span / n)
            out.append(f"{name} edge {pos - 0.5:.1f} / centre {pos:.1f}")
        print(f"  stage {stage}: centre {r['fft_centre']}, peak bin {r['peak_bin']}, {r['fps']:.1f} frames/s; " + "; ".join(out))

    print("\n-- dB scaling: byte = 255 * (dB - (offset - range)) / range under jp's mapping")
    for r in s["scaling"]:
        print(f"  offset {r['offset']:4d} range {r['range']:4d}: peak byte {r['peak_byte']:6.1f} floor byte {r['floor_byte']:6.1f}"
              f" -> peak {r['offset'] - r['range'] * (1 - r['peak_byte'] / 255):7.1f} dB,"
              f" floor {r['offset'] - r['range'] * (1 - r['floor_byte'] / 255):7.1f} dB")
    print("\n-- pixels requested -> received", s["pixels"])
    print("-- limit probe", s["limit_probe"])


def iq_spectrum_db(iq, rate, nfft=8192):
    """Welch power spectrum in dB, frequencies from -rate/2."""
    segs = [iq[k:k + nfft] for k in range(0, len(iq) - nfft, nfft // 2)]
    w = np.hanning(nfft)
    p = np.mean([np.abs(np.fft.fftshift(np.fft.fft(x * w))) ** 2 for x in segs], axis=0)
    f = (np.arange(nfft) - nfft / 2) * rate / nfft
    return f, 10 * np.log10(p + 1e-30)


def frames_between(data, t0, t1):
    sel = [(i, int(n)) for i, (t, n) in enumerate(zip(data["fft_t"], data["fft_len"])) if t0 <= t <= t1]
    if not sel:
        return None
    n = max(set(x[1] for x in sel), key=[x[1] for x in sel].count)
    rows = [data["fft"][i, :n].astype(np.float64) for i, m in sel if m == n]
    return np.mean(rows, axis=0)


def main2(prefix):
    meta, data = load(prefix)
    s = meta["summary"]
    dev = s["device"]
    iq = data["iq"].astype(np.complex64)
    rate = s["iq_rate"]
    sr, bw = dev["MaximumSampleRate"], dev["MaximumBandwidth"]
    print(f"{s['host']}:{s['port']} max SR {sr} max BW {bw} (BW/SR {bw / sr:.4f}), IQ rate {rate}")

    print("\n-- FFT span per stage: fit of the FFT frame to the IQ spectrum")
    print("   span = k * SR/2^stage; bin i centred at (i + 0.5 + d - n/2) * span/n (d = 0 centre, -0.5 edge convention)")
    for m in [m for m in s["marks"] if m["label"] == "stage"]:
        fr = frames_between(data, m["t0"], m["t1"])
        if fr is None:
            print(f"  stage {m['stage']}: no frames")
            continue
        n = len(fr)
        f_iq, p_iq = iq_spectrum_db(iq[m["iq0"]:m["iq1"]], rate)
        best = (-2, None, None)
        for k in np.arange(0.70, 1.10, 0.0025):
            span = k * sr / (1 << m["stage"])
            for d in np.arange(-3.0, 3.01, 0.25):
                fb = (np.arange(n) + 0.5 + d - n / 2) * span / n
                use = np.abs(fb) < 0.42 * rate
                if use.sum() < 40:
                    continue
                pred = np.interp(fb[use], f_iq, p_iq)
                r = np.corrcoef(pred, fr[use])[0, 1]
                if r > best[0]:
                    best = (r, k, d)
        r, k, d = best
        if k is None:
            print(f"  stage {m['stage']}: {n} bins, too few of them inside the IQ band to fit")
            continue
        print(f"  stage {m['stage']}: {n} bins, best k {k:.4f} (BW/SR {bw / sr:.4f}), shift {d:+.2f} bins, r {r:.3f}")

    print("\n-- dB scaling against the first setting: byte = a * byte_ref + c over bins unclipped in both")
    print("   jp's mapping dB = offset - range * (1 - byte/255) predicts a = range_ref/range,")
    print("   c = 255 * (offset_ref - offset + range - range_ref) / range ... see the doc")
    scales = [m for m in s["marks"] if m["label"] == "scale"]
    ref = frames_between(data, scales[0]["t0"], scales[0]["t1"]) if scales else None
    for m in scales:
        fr = frames_between(data, m["t0"], m["t1"])
        if fr is None or ref is None:
            print(f"  offset {m['offset']:3d} range {m['range']:3d}: no frames")
            continue
        use = (ref > 3) & (ref < 252) & (fr > 3) & (fr < 252)
        stats = f"median {np.median(fr):6.1f}, p99 {np.percentile(fr, 99):6.1f}, max {fr.max():6.1f}"
        if use.sum() < 10:
            print(f"  offset {m['offset']:3d} range {m['range']:3d}: {use.sum()} unclipped bins; {stats}")
            continue
        a, c = np.polyfit(ref[use], fr[use], 1)
        print(f"  offset {m['offset']:3d} range {m['range']:3d}: a {a:.3f} (ref/range {scales[0]['range'] / m['range']:.3f}),"
              f" c {c:+7.2f} bytes, {use.sum()} bins; {stats}")

    print("\n-- IQ placement (device centre", s["centre"], "): IQ spectrum against the stage-0 FFT at the same frequencies")
    bw = dev["MaximumBandwidth"]
    for m in [m for m in s["marks"] if m["label"] == "place"]:
        seg = iq[m["iq0"]:m["iq1"]]
        rms = 20 * np.log10(np.sqrt(np.mean(np.abs(seg) ** 2)) + 1e-9) if len(seg) else float("nan")
        fr = frames_between(data, m["t0"], m["t1"])
        corr = "no FFT"
        if fr is not None and len(seg) > 16384:
            n = len(fr)
            f_iq, p_iq = iq_spectrum_db(seg, rate)
            absf = m["iq_freq"] + f_iq                                   # absolute frequency of each IQ bin
            fb = s["centre"] + (np.arange(n) - n / 2) * bw / n           # FFT bin centres (bin n/2 on the centre)
            use = (absf > fb[0]) & (absf < fb[-1]) & (np.abs(f_iq) < 0.42 * rate)
            if use.sum() > 200:
                pred = np.interp(absf[use], fb, fr)
                corr = f"r {np.corrcoef(pred, p_iq[use])[0, 1]:.3f} over {use.sum()} IQ bins"
            else:
                corr = f"outside the FFT span ({use.sum()} IQ bins inside)"
        print(f"  IQ at centre + {m['frac']:.2f} * SR ({m['iq_freq']}): RMS {rms:.1f} dB, {corr}")
    print("\n-- syncs")
    for x in s["syncs"]:
        print("  ", {k: x[k] for k in ("DeviceCenterFrequency", "IQCenterFrequency", "FFTCenterFrequency",
                                       "MinimumIQCenterFrequency", "MaximumIQCenterFrequency")})


if __name__ == "__main__":
    (main2 if "main2_" in sys.argv[1] else main)(sys.argv[1])
