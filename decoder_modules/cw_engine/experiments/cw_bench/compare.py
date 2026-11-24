#!/usr/bin/env python3
"""compare.py — two decoders over many conditions, for DeepCW.md's quality
comparison (v3 against the DeepCW v2 narrow model).

Runs cw_engine_bench over a grid of conditions with the same seeds for every
decoder (so each over is the same text, keying and noise for both), and
appends one JSON line per over to the results file. A rerun skips the jobs
already in the file. compare_plot.py draws the figures from it.

    compare.py --bin <cw_engine_bench> --fused <weights> --out results.jsonl [--overs N] [--jobs J] [--only GROUP]

Decoders: "v3", and "dcw2n" on the fused kernel with the stitcher at
L 1 s, G 2 s, S 3 s (SKIM_DEEPCW2_LEFT / _GUARD / _STEP). Standard library
only.

Part of SDRIAK. GPL-3.0-or-later.
"""
import argparse
import concurrent.futures as cf
import json
import os
import subprocess
import sys
import threading
import time

DECODERS = {
    'v3': {},
    'dcw2n': {'SKIM_DEEPCW2_LEFT': '1', 'SKIM_DEEPCW2_GUARD': '2', 'SKIM_DEEPCW2_STEP': '3'},
}


def frange(a, b, step):
    out, x = [], a
    while x <= b + 1e-9:
        out.append(round(x, 2))
        x += step
    return out


SNR_AWGN = frange(-12, 20, 1)
SNR_FADE = frange(-9, 30, 1.5)
SNR_MID = frange(-9, 20, 1)
SNR_POS = frange(-9, 8, 1) + [12, 16, 20]
POSITIONS = [round(3.125 * k, 3) for k in range(21)] + [-6.25, -15.625, -31.25, -46.875, -62.5]

IMPULSES = [
    ('none', []),
    ('rx 1/s 40 dB', ['--impulse', 'poisson:1:40:rx']),
    ('sferic 3/s 40 dB', ['--impulse', 'poisson:3:40:sferic']),
    ('rx 10/s 40 dB', ['--impulse', 'poisson:10:40:rx']),
    ('ignition 50/s 30 dB', ['--impulse', 'periodic:50:30:rx']),
    ('rasp 100/s 25 dB', ['--impulse', 'periodic:100:25:rx']),
    ('crashes 0.2/s 40 dB', ['--impulse', 'crash:0.2:40']),
    ('Class A 0.1/0.01', ['--classa', '0.1:0.01']),
    ('rx 3/s 70 dB, clip 60', ['--impulse', 'poisson:3:70:rx', '--clip', '60']),
]


def conditions():
    """(group, label, wpm, snrs, extra args, overs scale)"""
    c = []
    for w in (10, 12, 15, 18, 22, 25, 30, 35, 40, 45):
        c.append(('speed', f'{w} WPM', w, SNR_AWGN, [], 1.0))
    for j in (0.0, 0.03, 0.06, 0.1, 0.15, 0.2, 0.3):
        c.append(('jitter', f'±{j * 100:.0f} %', 24, SNR_AWGN, ['--jitter', str(j)], 1.0))
    for f in ('none', 'itu:good', 'itu:moderate', 'itu:poor', 'itu:lowlat', 'itu:highlat', 'itu:highlat-poor',
              'flat:0.1', 'flat:0.3', 'flat:1', 'flat:3', 'flat:10'):
        c.append(('fading', f, 24, SNR_FADE, ['--fade', f], 1.0))
    for w in (12, 35):
        for f in ('itu:poor', 'flat:1', 'flat:3'):
            c.append(('fading_speed', f'{f}, {w} WPM', w, SNR_FADE, ['--fade', f], 1.0))
    for name, args in IMPULSES:
        c.append(('impulse', name, 24, SNR_MID, args, 1.0))
    for name, args in (('none', []),
                       ('20 stations, 0–20 dB', ['--qrm', '20:20:20']),
                       ('60 stations, 0–30 dB', ['--qrm', '60:30:30']),
                       ('co-channel −10 dB, 50 Hz', ['--cochannel', '-10:50']),
                       ('co-channel −6 dB, 100 Hz', ['--cochannel', '-6:100'])):
        c.append(('interference', name, 24, SNR_MID, args, 1.0))
    for off in (0, 15, 30, 45, 60):
        c.append(('offset', f'{off} Hz', 24, SNR_MID, ['--offset', str(off)], 0.6))
    for name, args in (('none', []),
                       ('drift 0.5 Hz/s', ['--drift', '0.5']), ('drift 1 Hz/s', ['--drift', '1']),
                       ('drift 2 Hz/s', ['--drift', '2']),
                       ('chirp 10 Hz, 20 ms', ['--chirp', '10:20']), ('chirp 30 Hz, 10 ms', ['--chirp', '30:10']),
                       ('18 → 30 WPM', ['--wpm-step', '30']), ('30 → 18 WPM', ['--wpm-step', '18'])):
        w = 30 if name.startswith('30') else (18 if name.startswith('18') else 24)
        c.append(('keying', name, w, SNR_MID, args, 0.6))
    # what is sent: random letter groups (no structure a decoder could lean
    # on), random alphanumeric groups, ham exchanges (CQ, contest, ragchew,
    # sign-off), and the bench's default payload (shuffled traffic words and
    # callsign-like tokens)
    for text, tname in (('letters', 'letter groups 2–6'), ('random', 'alphanumeric groups 2–6'),
                        ('qso', 'ham exchanges'), ('payload', 'words and calls (default)')):
        for fade, fname in (('none', 'AWGN'), ('itu:poor', 'ITU poor'), ('flat:3', 'flat 3 Hz')):
            args = ['--text', text] + ([] if fade == 'none' else ['--fade', fade])
            c.append(('text', f'{tname}, {fname}', 24, SNR_AWGN if fade == 'none' else SNR_FADE, args, 1.0))
        for w in (12, 35):
            c.append(('text_speed', f'{tname}, {w} WPM', w, SNR_AWGN, ['--text', text], 0.6))
    # the impulse group again with ImpulseRepair on the wideband stream before
    # the channelizer (cw_bench --repair, ALGORITHM.md §5.7), and two crowded
    # bands to check what the repair costs where there are no impulses
    for name, args in IMPULSES + [('20 stations, 0–20 dB', ['--qrm', '20:20:20']),
                                  ('60 stations, 0–30 dB', ['--qrm', '60:30:30'])]:
        c.append(('impulse_repair', name, 24, SNR_MID, args + ['--repair'], 1.0))
    # where the tone sits in its 125 Hz channel, in quarter bins of DeepCW's
    # 12.5 Hz DFT up to the channel boundary (62.5 Hz); with the skimmer's
    # default channel prototype (−6 dB at the boundary) and with the flat one
    # (SkimChannelizer::create_ex(…, 85, 12), flat to ±62.5 Hz)
    for proto, gname, args in (('default', 'position', []),
                               ('flat', 'position_flat', ['--passband', '85', '--taps', '12'])):
        for off in POSITIONS:
            c.append((gname, f'{off:g} Hz', 24, SNR_POS, ['--offset', str(off)] + args, 1 / 3))
    return c


NOISE = [(name, args) for name, args in IMPULSES] + [('60 stations, 0–30 dB', ['--qrm', '60:30:30'])]
NOISE += [(f'{name}, repair', args + ['--repair']) for name, args in NOISE]


def jobs(overs, chunk):
    out = []
    for dec in DECODERS:
        for group, label, wpm, snrs, extra, scale in conditions():
            n = max(1, int(round(overs * scale)))
            for s0 in range(0, n, chunk):
                out.append(dict(kind='cer', dec=dec, group=group, label=label, wpm=wpm, snrs=snrs, extra=extra,
                                seed0=s0, seeds=min(chunk, n - s0)))
        for label, extra in NOISE:
            for s0 in range(0, 6):
                out.append(dict(kind='noise', dec=dec, group='false', label=label, wpm=24, snrs=[], extra=extra,
                                seed0=s0, seeds=1))
    return out


def key(j):
    return f"{j['kind']}|{j['dec']}|{j['group']}|{j['label']}|{j['seed0']}|{j['seeds']}"


def run(binary, fused, j):
    env = dict(os.environ)
    env['SKIM_DEEPCW2_FUSED'] = fused
    env.update(DECODERS[j['dec']])
    args = [binary, '--cfg', j['dec'], '--wpm', str(j['wpm']), '--lead', '2', '--tail', '6',
            '--seed0', str(j['seed0']), '--seeds', str(j['seeds'])] + j['extra']
    if j['kind'] == 'noise':
        args += ['--noise-only', '600']
    else:
        args += ['--snr', ','.join(map(str, j['snrs']))]
    out = subprocess.run(args, capture_output=True, text=True, env=env)
    if out.returncode != 0:
        raise RuntimeError(f"{' '.join(args)}: {out.stderr[-500:]}")
    rows = []
    for line in out.stdout.splitlines():
        p = line.split()
        if j['kind'] == 'noise':
            # "<cfg> noise <seed> <seconds> s <chars> chars"
            if len(p) == 7 and p[1] == 'noise' and p[4] == 's' and p[6] == 'chars':
                rows.append(dict(seconds=float(p[3]), chars=int(p[5])))
        elif len(p) == 9 and not line.startswith(' '):
            rows.append(dict(snr=float(p[3]), seed=int(p[4]), n=int(p[5]), d=int(p[6]), pn=int(p[7]), pd=int(p[8])))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--bin', required=True)
    ap.add_argument('--fused', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--overs', type=int, default=100, help='overs a point (scaled per group)')
    ap.add_argument('--chunk', type=int, default=20, help='overs per cw_bench invocation')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument('--only', help='run only this group')
    a = ap.parse_args()
    done = set()
    if os.path.exists(a.out):
        with open(a.out) as f:
            for line in f:
                done.add(json.loads(line)['key'])
    todo = [j for j in jobs(a.overs, a.chunk) if key(j) not in done and (not a.only or j['group'] == a.only)]
    print(f'{len(todo)} jobs to run ({len(done)} done)', flush=True)
    lock = threading.Lock()
    t0 = time.time()
    finished = 0
    with open(a.out, 'a') as f, cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(run, a.bin, a.fused, j): j for j in todo}
        for fut in cf.as_completed(futs):
            j = futs[fut]
            rec = dict(key=key(j), **{k: j[k] for k in ('kind', 'dec', 'group', 'label', 'wpm', 'seed0', 'seeds')},
                       rows=fut.result())
            with lock:
                f.write(json.dumps(rec) + '\n')
                f.flush()
                finished += 1
                if finished % 50 == 0 or finished == len(todo):
                    el = time.time() - t0
                    print(f'{finished}/{len(todo)} jobs, {el / 60:.1f} min, about {el / finished * (len(todo) - finished) / 60:.0f} min left',
                          flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
