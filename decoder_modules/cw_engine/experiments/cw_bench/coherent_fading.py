"""Coherent element evidence under HF fading: one-element detection.

Reproduces the table in ALGORITHM.md §5.3.4. Needs numpy and scipy (unlike
bench.py, which is standard library only). It takes about 25 minutes:
each threshold is a 12-step bisection over 4000 trials.

Complex baseband at 250 Hz, the tone derotated by a perfect AFC (unless
`dfreq` is set), complex white noise. The element's complex gain follows a
Watterson channel inside the 125 Hz channel (one complex Gaussian process per
path, Gaussian Doppler spectrum of spread B, optional Doppler shift; delay
spread is irrelevant here). Reports the mean C/N0 (dB-Hz) for AUC 0.90;
SNR500 = C/N0 - 27 dB.

Statistics, all over the element's samples:
  v3     coherent sums of L = round(dit/2) (2..10), |.|^2 summed (today)
  cohT   |sum z| over the whole element
  blk    coherent blocks of min(T, 0.213/B) (|rho| >= 0.8 inside), |.|^2 summed
  genie  Re(sum z * conj(g)) with the true gain: perfect channel compensation
"""
import sys
import numpy as np
from scipy.signal import fftconvolve

FS = 250.0
NTR = 4000
rng = np.random.default_rng(3)


def gains(n, paths):
    """(NTR, n) complex gains, unit mean power; paths = [(power, B, shift)]"""
    g = np.zeros((NTR, n), complex)
    t = np.arange(n) / FS
    for p, B, nu in paths:
        if B <= 0:
            ph = rng.uniform(0, 2 * np.pi, (NTR, 1))
            g += np.sqrt(p) * np.exp(1j * (ph + 2 * np.pi * nu * t))
            continue
        st = np.sqrt(2) / (2 * np.pi * B) * FS
        half = int(np.ceil(4 * st))
        k = np.exp(-0.5 * (np.arange(-half, half + 1) / st) ** 2)
        k /= np.sqrt((k ** 2).sum())
        w = (rng.standard_normal((NTR, n + 2 * half)) + 1j * rng.standard_normal((NTR, n + 2 * half))) / np.sqrt(2)
        x = fftconvolve(w, k[None, :], mode='valid', axes=1)
        g += np.sqrt(p) * x * np.exp(2j * np.pi * nu * t)[None, :]
    return g


def auc(x1, x0):
    a = np.concatenate([x1, x0]); r = a.argsort().argsort()
    return (r[:len(x1)].sum() - len(x1) * (len(x1) - 1) / 2) / (len(x1) * len(x0))


def stats(z, g, n, dit_n, B):
    L = int(np.clip(round(0.5 * dit_n), 2, 10))
    c = np.cumsum(np.pad(z, ((0, 0), (1, 0))), axis=1)
    v3 = (np.abs(c[:, L:] - c[:, :-L]) ** 2).sum(axis=1)
    coh = np.abs(z.sum(axis=1))
    lb = n if B <= 0 else max(1, min(n, int(0.213 / B * FS)))
    nb = n // lb
    blk = (np.abs(z[:, :nb * lb].reshape(len(z), nb, lb).sum(axis=2)) ** 2).sum(axis=1)
    if nb * lb < n:                       # the remainder as a last, shorter block
        blk = blk + np.abs(z[:, nb * lb:].sum(axis=1)) ** 2
    genie = np.real((z * np.conj(g)).sum(axis=1)) if g is not None else None
    return dict(v3=v3, cohT=coh, blk=blk, genie=genie)


def score(cn0, n, dit_n, paths, dfreq):
    a = np.sqrt(10 ** (cn0 / 10) / FS)
    g = gains(n, paths) * np.exp(2j * np.pi * dfreq * np.arange(n) / FS)[None, :]
    nz = lambda: (rng.standard_normal((NTR, n)) + 1j * rng.standard_normal((NTR, n))) / np.sqrt(2)
    B = max([p[1] for p in paths] + [0.0])
    B_eff = max(B, abs(paths[0][2] - paths[-1][2]) if len(paths) > 1 else 0.0)
    s1 = stats(a * g + nz(), a * g, n, dit_n, B_eff)
    s0 = stats(nz(), a * g, n, dit_n, B_eff)
    return {k: auc(s1[k], s0[k]) for k in s1}


def thresholds(n, dit_n, paths, dfreq=0.0, target=0.90):
    out = {}
    for key in ('v3', 'cohT', 'blk', 'genie'):
        lo, hi = 0.0, 55.0
        for _ in range(12):
            mid = 0.5 * (lo + hi)
            if score(mid, n, dit_n, paths, dfreq)[key] < target:
                lo = mid
            else:
                hi = mid
        out[key] = 0.5 * (lo + hi)
    return out


CASES = [
    ('steady carrier (VHF, line of sight)', [(1, 0, 0)], 0.0),
    ('steady, AFC 2 Hz off', [(1, 0, 0)], 2.0),
    ('flat 0.1 Hz (ITU quiet)', [(1, 0.1, 0)], 0.0),
    ('flat 0.5 Hz (ITU moderate)', [(1, 0.5, 0)], 0.0),
    ('flat 1 Hz (ITU disturbed)', [(1, 1.0, 0)], 0.0),
    ('flat 3 Hz', [(1, 3.0, 0)], 0.0),
    ('two paths +-0.5 Hz, 0.1 Hz each (beating)', [(0.5, 0.1, 0.5), (0.5, 0.1, -0.5)], 0.0),
    ('flat 10 Hz (high latitude)', [(1, 10.0, 0)], 0.0),
]

if __name__ == '__main__':
    elems = [('dit 25 WPM (48 ms)', 12, 12), ('dah 12 WPM (300 ms)', 75, 25)]
    print("mean C/N0 (dB-Hz) for AUC 0.90 on one element; lower is better; SNR500 = C/N0 - 27")
    for ename, n, dit_n in elems:
        print(f"\n{ename}")
        print(f"  {'channel':44s} {'v3':>6s} {'cohT':>6s} {'blk':>6s} {'genie':>6s}   cohT-v3  blk-v3  genie-v3")
        for name, paths, df in CASES:
            t = thresholds(n, dit_n, paths, df)
            print(f"  {name:44s} {t['v3']:6.1f} {t['cohT']:6.1f} {t['blk']:6.1f} {t['genie']:6.1f}"
                  f"   {t['v3']-t['cohT']:+6.1f}  {t['v3']-t['blk']:+6.1f}  {t['v3']-t['genie']:+7.1f}", flush=True)
