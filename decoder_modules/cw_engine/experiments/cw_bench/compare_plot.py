#!/usr/bin/env python3
"""compare_plot.py — figures and tables from compare.py's results.

    compare_plot.py results.jsonl <figure directory>

Writes one PNG per group (payload CER against SNR500, one panel per
condition, the two decoders in each), the threshold figures (SNR500 at 10 %
and 3 % payload CER) for speed and keying jitter, the false-character
figure, the channel-position and impulse-repair figures, points.csv (the points drawn: characters and errors; the false
characters as seconds and characters), and prints the markdown tables
DeepCW.md quotes. Needs matplotlib.

Part of SDRIAK. GPL-3.0-or-later.
"""
import collections
import json
import math
import os
import sys

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

# The reference palette's categorical slots 1 and 2 (light surface), with
# distinct markers so the decoders differ without colour too.
SURFACE, INK, INK2, GRID = '#fcfcfb', '#0b0b0b', '#52514e', '#e6e5e1'
STYLE = {
    'v3': dict(color='#2a78d6', marker='o', label='v3 (OK1HRA)'),
    'dcw2n': dict(color='#eb6834', marker='s', label='DeepCW v2 narrow, L 1 G 2 S 3'),
}
ORDER = ['v3', 'dcw2n']

plt.rcParams.update({
    'figure.facecolor': SURFACE, 'axes.facecolor': SURFACE, 'savefig.facecolor': SURFACE,
    'axes.edgecolor': GRID, 'axes.labelcolor': INK2, 'xtick.color': INK2, 'ytick.color': INK2,
    'text.color': INK, 'axes.grid': True, 'grid.color': GRID, 'grid.linewidth': 0.8, 'grid.linestyle': '-',
    'axes.spines.top': False, 'axes.spines.right': False, 'font.size': 9, 'axes.titlesize': 9.5,
    'axes.titlecolor': INK, 'legend.frameon': False, 'lines.linewidth': 2.0, 'lines.markersize': 4.5,
})


def load(path):
    cer = collections.defaultdict(lambda: [0, 0])            # (group, label, dec, snr) -> [pn, pd]
    noise = collections.defaultdict(lambda: [0.0, 0])        # (label, dec) -> [seconds, chars]
    labels = collections.OrderedDict()                       # group -> [labels] in run order
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if r['kind'] == 'noise':
                for x in r['rows']:
                    n = noise[(r['label'], r['dec'])]
                    n[0] += x['seconds']
                    n[1] += x['chars']
                continue
            labels.setdefault(r['group'], [])
            if r['label'] not in labels[r['group']]:
                labels[r['group']].append(r['label'])
            for x in r['rows']:
                c = cer[(r['group'], r['label'], r['dec'], x['snr'])]
                c[0] += x['pn']
                c[1] += x['pd']
    return cer, noise, labels


def curve(cer, group, label, dec):
    pts = sorted((s, pd / pn, pn) for (g, l, d, s), (pn, pd) in cer.items()
                 if g == group and l == label and d == dec and pn)
    return [p[0] for p in pts], [p[1] for p in pts], [p[2] for p in pts]


def crossing(snrs, cers, level):
    """the first SNR (ascending) where CER falls to <= level, interpolated"""
    if not snrs:
        return math.nan
    if cers[0] <= level:
        return snrs[0]
    for (s0, c0), (s1, c1) in zip(zip(snrs, cers), zip(snrs[1:], cers[1:])):
        if c0 > level >= c1:
            return s0 + (c0 - level) / (c0 - c1) * (s1 - s0)
    return math.nan


def legend(fig):
    handles = [plt.Line2D([], [], **{k: v for k, v in STYLE[d].items()}) for d in ORDER]
    fig.legend(handles=handles, loc='upper center', ncol=2, bbox_to_anchor=(0.5, 1.0))


def panels(cer, labels, group, title, path, ncols=4, xlabel='SNR500 (dB)'):
    ls = labels.get(group, [])
    if not ls:
        return
    nrows = math.ceil(len(ls) / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(2.6 * ncols, 2.15 * nrows + 0.7), sharey=True, squeeze=False)
    for ax, label in zip(axes.flat, ls):
        for d in ORDER:
            x, y, _ = curve(cer, group, label, d)
            ax.plot(x, [100 * v for v in y], color=STYLE[d]['color'], marker=STYLE[d]['marker'], markevery=3)
        # a long label ("alphanumeric groups 2–6, ITU poor") on two lines
        ax.set_title(label.replace(', ', ',\n', 1) if len(label) > 26 else label, loc='left')
        ax.set_ylim(0, 100)
        ax.set_yticks([0, 25, 50, 75, 100])
    for ax in axes.flat[len(ls):]:
        ax.set_visible(False)
    for ax in axes[:, 0]:
        ax.set_ylabel('payload CER (%)')
    for ax in axes[-1, :]:
        ax.set_xlabel(xlabel)
    legend(fig)
    fig.suptitle(title, x=0.01, y=1.04, ha='left', fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(path, dpi=110, bbox_inches='tight')
    plt.close(fig)


def thresholds(cer, labels, group, xs, xlabel, title, path):
    fig, axes = plt.subplots(1, 2, figsize=(8.4, 3.1), sharey=True)
    for ax, level in zip(axes, (0.10, 0.03)):
        ends = {}
        for d in ORDER:
            ys = [crossing(*curve(cer, group, l, d)[:2], level) for l in labels[group]]
            ax.plot(xs, ys, color=STYLE[d]['color'], marker=STYLE[d]['marker'])
            ok = [(x, y) for x, y in zip(xs, ys) if not math.isnan(y)]
            if ok:
                ends[d] = ok[-1]
        # the values at the lines' ends, in ink; two that would touch are moved apart
        close = len(ends) == 2 and abs(ends['v3'][1] - ends['dcw2n'][1]) < 1.2
        for d, (x, y) in ends.items():
            dy = 0 if not close else (6 if y >= ends['v3' if d == 'dcw2n' else 'dcw2n'][1] and d == 'dcw2n' else -6)
            ax.annotate(f'{y:+.1f}', (x, y), xytext=(5, dy), textcoords='offset points', va='center', fontsize=8,
                        color=INK2)
        ax.set_title(f'SNR500 where payload CER falls to {level * 100:.0f} %', loc='left')
        ax.set_xlabel(xlabel)
    axes[0].set_ylabel('SNR500 (dB), lower is better')
    legend(fig)
    fig.suptitle(title, x=0.01, y=1.06, ha='left', fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(path, dpi=110, bbox_inches='tight')
    plt.close(fig)


def positions(cer, labels, path):
    """the 10 % and 3 % points against the tone's offset from the channel
    centre (0 to the boundary), the decoders by colour, the channel prototype
    by line: solid the default, dashed the flat one"""
    fig, axes = plt.subplots(1, 2, figsize=(8.4, 3.3), sharey=True)
    for ax, level in zip(axes, (0.10, 0.03)):
        for g, ls in (('position', '-'), ('position_flat', '--')):
            offs = sorted((float(l.split()[0]), l) for l in labels.get(g, []) if float(l.split()[0]) >= 0)
            for d in ORDER:
                ys = [crossing(*curve(cer, g, l, d)[:2], level) for _, l in offs]
                ax.plot([o for o, _ in offs], ys, color=STYLE[d]['color'], marker=STYLE[d]['marker'], markevery=4,
                        linestyle=ls)
        ax.axvline(62.5, color=INK2, linewidth=0.8)
        ax.annotate('channel boundary', (62.5, 1), xycoords=('data', 'axes fraction'), xytext=(-4, -4),
                    textcoords='offset points', ha='right', va='top', fontsize=8, color=INK2)
        ax.set_title(f'SNR500 where payload CER falls to {level * 100:.0f} %', loc='left')
        ax.set_xlabel('tone offset from the channel centre (Hz)')
        ax.set_xticks([0, 12.5, 25, 37.5, 50, 62.5])
    axes[0].set_ylabel('SNR500 (dB), lower is better')
    handles = [plt.Line2D([], [], color=STYLE[d]['color'], marker=STYLE[d]['marker'], label=STYLE[d]['label'])
               for d in ORDER]
    handles += [plt.Line2D([], [], color=INK2, linestyle=ls, label=t)
                for ls, t in (('-', 'default channel prototype'), ('--', 'flat prototype (create_ex 85 Hz, 12 taps)'))]
    fig.legend(handles=handles, loc='upper center', ncol=2, bbox_to_anchor=(0.5, 1.0))
    fig.suptitle('Where the tone sits in its 125 Hz channel, 24 WPM, AWGN', x=0.01, y=1.10, ha='left', fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.86))
    fig.savefig(path, dpi=110, bbox_inches='tight')
    plt.close(fig)


def position_table(cer, labels):
    rows = ['| offset | v3 10 % | dcw2n 10 % | v3 flat 10 % | dcw2n flat 10 % | v3 3 % | dcw2n 3 % | v3 flat 3 % | dcw2n flat 3 % |',
            '|---|---|---|---|---|---|---|---|---|']
    f = lambda v: '—' if math.isnan(v) else f'{v:+.1f}'
    for l in labels.get('position', []):
        cells = [f(crossing(*curve(cer, g, l, d)[:2], level))
                 for level in (0.10, 0.03) for g in ('position', 'position_flat') for d in ORDER]
        rows.append(f'| {l} | ' + ' | '.join(cells[i] for i in (0, 1, 2, 3, 4, 5, 6, 7)) + ' |')
    return '\n'.join(rows)


def base_group(labels, label):
    """the group holding a repaired condition's unrepaired twin"""
    return 'impulse' if label in labels.get('impulse', []) else 'interference'


def repair_panels(cer, labels, path, ncols=4):
    """the impulse_repair group, solid, over the same conditions without the
    repair, dashed"""
    ls = labels.get('impulse_repair', [])
    nrows = math.ceil(len(ls) / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(2.6 * ncols, 2.15 * nrows + 0.9), sharey=True, squeeze=False)
    for ax, label in zip(axes.flat, ls):
        for g, style in ((base_group(labels, label), dict(linestyle='--', linewidth=1.3, alpha=0.75)),
                         ('impulse_repair', dict(linestyle='-'))):
            for d in ORDER:
                x, y, _ = curve(cer, g, label, d)
                ax.plot(x, [100 * v for v in y], color=STYLE[d]['color'], marker=STYLE[d]['marker'], markevery=3,
                        **style)
        ax.set_title(label, loc='left')
        ax.set_ylim(0, 100)
        ax.set_yticks([0, 25, 50, 75, 100])
    for ax in axes.flat[len(ls):]:
        ax.set_visible(False)
    for ax in axes[:, 0]:
        ax.set_ylabel('payload CER (%)')
    for ax in axes[-1, :]:
        ax.set_xlabel('SNR500 (dB)')
    handles = [plt.Line2D([], [], color=STYLE[d]['color'], marker=STYLE[d]['marker'], label=STYLE[d]['label'])
               for d in ORDER]
    handles += [plt.Line2D([], [], color=INK2, linestyle='-', label='with impulse repair'),
                plt.Line2D([], [], color=INK2, linestyle='--', linewidth=1.3, label='without')]
    fig.legend(handles=handles, loc='upper center', ncol=4, bbox_to_anchor=(0.5, 1.0))
    fig.suptitle('Impulse repair before the channelizer (ImpulseRepair), 24 WPM', x=0.01, y=1.04, ha='left',
                 fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(path, dpi=110, bbox_inches='tight')
    plt.close(fig)


def repair_table(cer, labels):
    """10 % point and CER at the top SNR, the four combinations"""
    rows = ['| condition | v3 10 % | v3 + repair | dcw2n 10 % | dcw2n + repair | v3 top | v3 + repair top | dcw2n top | dcw2n + repair top |',
            '|---|---|---|---|---|---|---|---|---|']
    f = lambda v: '—' if math.isnan(v) else f'{v:+.1f}'
    for l in labels.get('impulse_repair', []):
        b = base_group(labels, l)
        ths, tops = [], []
        for d in ORDER:
            for g in (b, 'impulse_repair'):
                x, y, _ = curve(cer, g, l, d)
                ths.append(f(crossing(x, y, 0.10)))
                tops.append(f'{100 * y[-1]:.2f} %' if y else '—')
        rows.append(f'| {l} | ' + ' | '.join(ths + tops) + ' |')
    return '\n'.join(rows)


def false_chars(noise, path):
    ls = list(dict.fromkeys(l for l, _ in noise))
    if not ls:
        return
    fig, ax = plt.subplots(figsize=(7.5, 0.42 * len(ls) + 1.0))
    h = 0.38
    for i, d in enumerate(ORDER):
        vals = [3600.0 * noise[(l, d)][1] / noise[(l, d)][0] if noise[(l, d)][0] else 0.0 for l in ls]
        ys = [k + (i - 0.5) * (h + 0.04) for k in range(len(ls))]
        ax.barh(ys, vals, height=h, color=STYLE[d]['color'], label=STYLE[d]['label'], edgecolor=SURFACE, linewidth=2)
        for y, v in zip(ys, vals):
            ax.annotate(f'{v:.0f}', (v, y), xytext=(3, 0), textcoords='offset points', va='center', fontsize=8,
                        color=INK2)
    ax.set_axisbelow(True)
    ax.set_yticks(range(len(ls)))
    ax.set_yticklabels(ls)
    ax.invert_yaxis()
    ax.set_xlabel('false characters per hour (no signal, 1 h a condition and decoder)')
    ax.grid(axis='y', visible=False)
    ax.legend(loc='lower right')
    ax.set_title('Text decoded from no signal', loc='left', fontsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=110, bbox_inches='tight')
    plt.close(fig)


def table(cer, labels, group):
    rows = ['| condition | v3 10 % | dcw2n 10 % | Δ | v3 3 % | dcw2n 3 % | Δ |', '|---|---|---|---|---|---|---|']
    for l in labels.get(group, []):
        cells = []
        for level in (0.10, 0.03):
            a = crossing(*curve(cer, group, l, 'v3')[:2], level)
            b = crossing(*curve(cer, group, l, 'dcw2n')[:2], level)
            f = lambda v: '—' if math.isnan(v) else f'{v:+.1f}'
            cells += [f(a), f(b), '—' if math.isnan(a) or math.isnan(b) else f'{b - a:+.1f}']
        rows.append(f'| {l} | ' + ' | '.join(cells) + ' |')
    return '\n'.join(rows)


def main():
    cer, noise, labels = load(sys.argv[1])
    # the conditions in compare.py's order, not the order the jobs finished in
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import compare
    want = collections.OrderedDict()
    for g, l, *_ in compare.conditions():
        want.setdefault(g, []).append(l)
    labels = collections.OrderedDict((g, [l for l in ls if l in labels.get(g, [])]) for g, ls in want.items())
    nl = [l for l, _ in compare.NOISE]
    noise = collections.OrderedDict(sorted(noise.items(), key=lambda kv: (nl.index(kv[0][0]) if kv[0][0] in nl else 99)))
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    titles = {
        'speed': 'Keying speed, AWGN',
        'jitter': 'Keying jitter (every element ± the share), 24 WPM, AWGN',
        'fading': 'Fading (ITU-R F.1487 two-path Watterson; flat = one path), 24 WPM',
        'fading_speed': 'Fading at 12 and 35 WPM',
        'impulse': 'Impulse noise, 24 WPM',
        'interference': 'Other stations (adjacent channels; co-channel), 24 WPM',
        'offset': 'Tone offset from the channel centre, 24 WPM',
        'keying': 'Drift, chirp, a speed change mid-over',
        'text': 'What is sent, 24 WPM',
        'text_speed': 'What is sent, at 12 and 35 WPM, AWGN',
    }
    for g, t in titles.items():
        panels(cer, labels, g, t, os.path.join(out, f'{g}.png'), ncols=3 if g in ('text', 'fading_speed') else 4)
    if 'speed' in labels:
        thresholds(cer, labels, 'speed', [int(l.split()[0]) for l in labels['speed']], 'WPM',
                   'Sensitivity by keying speed, AWGN', os.path.join(out, 'speed_thresholds.png'))
    if 'jitter' in labels:
        thresholds(cer, labels, 'jitter', [float(l.strip('±% ')) for l in labels['jitter']], 'keying jitter (± %)',
                   'Sensitivity by keying jitter, 24 WPM, AWGN', os.path.join(out, 'jitter_thresholds.png'))
    false_chars(noise, os.path.join(out, 'false.png'))
    if labels.get('impulse_repair'):
        repair_panels(cer, labels, os.path.join(out, 'impulse_repair.png'))
    if labels.get('position'):
        positions(cer, labels, os.path.join(out, 'position.png'))
    # the points the figures are drawn from, small enough to keep beside them
    with open(os.path.join(out, 'points.csv'), 'w') as f:
        f.write('group,condition,decoder,snr500_db,payload_chars,payload_errors\n')
        for g, ls in labels.items():
            for l in ls:
                for d in ORDER:
                    for s in sorted(s for (g2, l2, d2, s) in cer if (g2, l2, d2) == (g, l, d)):
                        pn, pd = cer[(g, l, d, s)]
                        f.write(f'{g},"{l}",{d},{s:g},{pn},{pd}\n')
        for (l, d), (sec, ch) in noise.items():
            f.write(f'false,"{l}",{d},,{sec:g},{ch}\n')         # seconds of noise, characters
    n = collections.Counter()
    for (g, l, d, s), (pn, pd) in cer.items():
        n[d] += pn
    print(f'payload characters scored: ' + ', '.join(f'{d} {n[d]}' for d in ORDER))
    for g in titles:
        if g in labels:
            print(f'\n### {g}\n')
            print(table(cer, labels, g))
    if labels.get('impulse_repair'):
        print('\n### impulse repair (10 % point; CER at +20 dB)\n')
        print(repair_table(cer, labels))
    if labels.get('position'):
        print('\n### position (default and flat channel prototype)\n')
        print(position_table(cer, labels))
    print('\n### false characters per hour\n')
    print('| condition | v3 | dcw2n |\n|---|---|---|')
    for l in dict.fromkeys(l for l, _ in noise):
        f = lambda d: 3600.0 * noise[(l, d)][1] / noise[(l, d)][0] if noise[(l, d)][0] else math.nan
        print(f'| {l} | {f("v3"):.1f} | {f("dcw2n"):.1f} |')
    return 0


if __name__ == '__main__':
    sys.exit(main())
