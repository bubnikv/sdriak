#!/usr/bin/env python3
"""bench.py — named measurement cases for cw_bench, with report tables.

Runs cw_engine_bench (built with OPT_BUILD_TESTS) in parallel and prints the
tables ALGORITHM.md quotes. Standard library only.

    bench.py --bin <path to cw_engine_bench> <case> [options] [-- extra cw_bench args]

Cases:
    ladder    SNR500 at CER 10 / 3 / 1 % by speed (payload scored)
    offset    SNR500 at CER 10 % by tone offset from the channel centre
    startup   whole-over CER and failed overs at high SNR by noise lead-in
    fading    payload CER by ITU-R F.1487 condition and flat Doppler spread,
              at the SNR500 points of DeepCW's CW Skimmer table
    impulse   payload CER with impulse noise, and false characters on noise
    noise     false characters per hour on noise (and on --bed when given)

Decoder switches go through the environment, as for the gates, e.g.
    SKIM_V3_SETTLE_S=0.5 bench.py --bin ... startup
Several --cfg give one column or row block each (v3, v2, v2:<narrow fc>; with
OPT_BUILD_CW_DEEPCW also dcw1, dcw2, dcw2n and their :nogate variants, the
models from SKIM_DEEPCW_MODEL, SKIM_DEEPCW2_MODEL, SKIM_DEEPCW2_NARROW_MODEL —
pass `-- --tail 4` so the DeepCW backends, which commit behind the live
edge, see the end of every over).
"""
import argparse
import concurrent.futures as cf
import collections
import os
import subprocess
import sys


def run(binary, args):
    out = subprocess.run([binary] + args, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"cw_bench failed: {' '.join(args)}\n{out.stderr}")
    return out.stdout


def parse(text):
    """cfg wpm offset snr seed ref_len dist pay_len pay_dist -> list of dicts"""
    rows = []
    for line in text.splitlines():
        p = line.split()
        if len(p) != 9 or line.startswith(' '):
            continue
        rows.append(dict(cfg=p[0], wpm=float(p[1]), off=float(p[2]), snr=float(p[3]), seed=int(p[4]),
                         n=int(p[5]), d=min(int(p[6]), int(p[5])), pn=int(p[7]), pd=int(p[8])))
    return rows


def cer(rows, payload=True):
    n = sum(r['pn'] if payload else r['n'] for r in rows)
    d = sum(r['pd'] if payload else r['d'] for r in rows)
    return d / n if n else float('nan')


def crossing(points, level):
    """first SNR (ascending) where CER falls to <= level, interpolated"""
    pts = sorted(points)
    if pts and pts[0][1] <= level:
        return pts[0][0]
    for (s0, c0), (s1, c1) in zip(pts, pts[1:]):
        if c0 > level >= c1:
            return s0 + (c0 - level) / (c0 - c1) * (s1 - s0)
    return float('nan')


def fan_out(binary, jobs, arglists):
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        return [parse(t) for t in ex.map(lambda a: run(binary, a), arglists)]


def snr_range(cfg):
    if cfg.startswith('dcw'):          # the DeepCW backends read well below v2/v3
        return '-12:8'
    return '-5:8' if cfg.startswith('v3') else '0:10'


def case_ladder(a):
    wpms = [12, 18, 25, 35]
    tasks = [(c, w) for c in a.cfg for w in wpms]
    res = fan_out(a.bin, a.jobs, [['--cfg', c, '--wpm', str(w), '--snr', snr_range(c), '--seeds', str(a.seeds)]
                                  + a.extra for c, w in tasks])
    by = {t: r for t, r in zip(tasks, res)}
    print(f"SNR500 (dB) where payload CER falls to the level ({a.seeds} overs a point)")
    for lev in (0.10, 0.03, 0.01):
        print(f"\n  CER {lev*100:.0f} %      " + "  ".join(f"{w:>2} WPM" for w in wpms))
        for c in a.cfg:
            cells = []
            for w in wpms:
                pts = collections.defaultdict(list)
                for r in by[(c, w)]:
                    pts[r['snr']].append(r)
                cells.append(crossing([(s, cer(v)) for s, v in pts.items()], lev))
            print(f"  {c:12s}  " + "  ".join(f"{x:6.2f}" for x in cells))


def case_offset(a):
    offs = [0, 20, 40, 50, 55, 62.5]
    tasks = [(c, o) for c in a.cfg for o in offs]
    res = fan_out(a.bin, a.jobs, [['--cfg', c, '--wpm', '25', '--offset', str(o), '--snr', snr_range(c),
                                   '--seeds', str(a.seeds)] + a.extra for c, o in tasks])
    print(f"SNR500 (dB) at payload CER 10 %, 25 WPM, by tone offset from the channel centre")
    print("  offset Hz     " + "  ".join(f"{o:6.1f}" for o in offs))
    by = {t: r for t, r in zip(tasks, res)}
    for c in a.cfg:
        cells = []
        for o in offs:
            pts = collections.defaultdict(list)
            for r in by[(c, o)]:
                pts[r['snr']].append(r)
            cells.append(crossing([(s, cer(v)) for s, v in pts.items()], 0.10))
        print(f"  {c:12s}  " + "  ".join(f"{x:6.2f}" for x in cells))


def case_startup(a):
    leads, wpms = [0.5, 5, 12], [25, 35]
    seeds = max(a.seeds, 20)
    tasks = [(c, l, w) for c in a.cfg for l in leads for w in wpms]
    res = fan_out(a.bin, a.jobs, [['--cfg', c, '--wpm', str(w), '--snr', '14', '--lead', str(l),
                                   '--seeds', str(seeds)] + a.extra for c, l, w in tasks])
    print(f"whole-over CER % (failed overs of {seeds}) at SNR500 14 dB, by noise lead-in")
    print("  cfg           " + "   ".join(f"lead {l:>4} s: 25 / 35 WPM" for l in leads))
    by = {t: r for t, r in zip(tasks, res)}
    for c in a.cfg:
        cells = []
        for l in leads:
            parts = []
            for w in wpms:
                rows = by[(c, l, w)]
                failed = sum(r['d'] > 0.3 * r['n'] for r in rows)
                parts.append(f"{100*cer(rows, payload=False):4.1f} ({failed})")
            cells.append(" / ".join(parts))
        print(f"  {c:12s}  " + "   ".join(f"{x:24s}" for x in cells))


def case_fading(a):
    fades = (a.fades.split() if a.fades else
             ['none', 'itu:good', 'itu:moderate', 'itu:poor', 'flat:0.1', 'flat:0.3', 'flat:1', 'flat:3'])
    snrs = [float(s) for s in a.snrs.split(',')] if a.snrs else [-12, -6, -3, 0, 6, 10, 20]
    tasks = [(c, f) for c in a.cfg for f in fades]
    res = fan_out(a.bin, a.jobs, [['--cfg', c, '--wpm', '24', '--fade', f, '--lead', '2',
                                   '--snr', ','.join(map(str, snrs)), '--seeds', str(a.seeds)] + a.extra
                                  for c, f in tasks])
    by = {t: r for t, r in zip(tasks, res)}
    print(f"payload CER at 24 WPM by fading and SNR500 ({a.seeds} overs a point;"
          f" DeepCW's published CW Skimmer rates use the same SNR points)")
    for c in a.cfg:
        print(f"\n  {c:14s}" + "".join(f"{s:>+7}" for s in snrs))
        for f in fades:
            cells = []
            for s in snrs:
                rows = [r for r in by[(c, f)] if r['snr'] == s]
                cells.append(f"{cer(rows):7.2f}")
            print(f"  {f:14s}" + "".join(cells))


def noise_rate(text):
    """characters per hour from a --noise-only run's total line"""
    last = text.strip().splitlines()[-1]
    return float(last.split(':', 1)[1].split()[0])


def case_impulse(a):
    imps = ['', 'poisson:1:40', 'poisson:3:40', 'poisson:10:40', 'periodic:50:30']
    tasks = [(c, i) for c in a.cfg for i in imps]
    def args(c, i):
        return ['--cfg', c] + (['--impulse', i] if i else []) + a.extra
    res = fan_out(a.bin, a.jobs, [args(c, i) + ['--wpm', '25', '--snr', '6', '--lead', '2', '--seeds', str(a.seeds)]
                                  for c, i in tasks])
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        noise = list(ex.map(lambda t: run(a.bin, args(*t) + ['--noise-only', '120', '--seeds', '3']), tasks))
    print("impulse noise (dB over the wideband noise rms): payload CER at 25 WPM, SNR500 6 dB,"
          f" {a.seeds} overs; false characters per hour on 6 minutes of noise")
    for c in a.cfg:
        print(f"\n  {c:6s} {'impulses':16s} {'CER':>7s}  {'false chars/h':>14s}")
        for (cc, i), rows, nz in zip(tasks, res, noise):
            if cc == c:
                print(f"  {'':6s} {i or 'none':16s} {100*cer(rows):6.1f} %  {noise_rate(nz):14.0f}")


def case_noise(a):
    extra = ['--bed', a.bed] if a.bed else []
    print(f"false characters per hour on 20 minutes of {'the bed' if a.bed else 'noise'}")
    for c in a.cfg:
        rate = noise_rate(run(a.bin, ['--cfg', c, '--noise-only', '300', '--seeds', '4'] + extra + a.extra))
        print(f"  {c:12s} {rate:8.0f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('case', choices=['ladder', 'offset', 'startup', 'fading', 'impulse', 'noise'])
    ap.add_argument('--bin', required=True, help='path to cw_engine_bench')
    ap.add_argument('--cfg', action='append', help='v3 (default), v2, v2:<fc> ...')
    ap.add_argument('--seeds', type=int, default=10)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--bed', help='cf32 IQ recording for the noise case')
    ap.add_argument('--fades', help='fading case: space-separated --fade values (default: ITU + flat grid)')
    ap.add_argument('--snrs', help='fading case: comma-separated SNR500 points')
    a, extra = ap.parse_known_args()
    a.extra = [x for x in extra if x != '--']
    a.cfg = a.cfg or ['v3']
    globals()['case_' + a.case](a)


if __name__ == '__main__':
    main()
