# Band stack — implementation, and a proposed model

Companion to `doc/research/band-stacking.md`, which establishes from primary
sources what a band-stack register actually contains (Icom CI-V `1A 01`,
Thetis `clsBandStackManager.cs`, Quisk `bandState`, Elecraft K4 `MA$`). Part 1
describes the band stack as built; Parts 2–7 propose the data model and state
machine to extend it; Part 8 describes the data-layer seam the proposal builds
on.

**What is built.** Plan rows receive stable `band_id` values and collapse to
canonical selector keys, and each band ID owns exactly three rotating optional
entries. Entry 0 is always current: ordinary write-back updates it in place.
While any entry is empty, repeat tap rotates the prefix through the first empty
entry right, then stores the current VFO at entry 0. Thus `[A, empty, empty] + X`
becomes `[X, A, empty]`, and the next repeat with `Y` becomes `[Y, X, A]`. Once
full, repeat tap overwrites entry 0 and rotates all three entries right. A
register-list pick materializes an empty row without first overwriting the old
top, then promotes the selected row by rotating its prefix. A populated pick
stores the current VFO before promotion. Empty positions retain their slot
identity outside the rotated prefix. No current pointer is stored. Opening
resolves only inside the restored visible group without writing; lifecycle save
is additionally locked to the current service. The service-owned catalog and
shadow/dwell tracking of Parts 2–7 are not built.

**Config keys.** Frequency-input preferences and memory state live in one
`frequencyMemory` subtree. The complete persisted layout is declared in
`gui/widgets/freq_memory.h` — no other file spells the key names out:

```text
frequencyMemory:
  version: 1
  page: "band" | "spectrum" | "keypad"
  selector: "band" | "spectrum"
  band:
    group: "ham" | "broadcast" | "air" | "marine" | "utility" | "all"
    preferredService: <BandService key>
    stackingRegisters: { <band ID>: [ <register|null>, ... x3 ] }
      <register>: { freq: <Hz number>, mode?: <canonical radio-mode string> }
  spectrum:
    activeRange: <spectrum range ID>
    lastFrequency: { <spectrum range ID>: <frequency> }
```

Keys outside this layout are ignored. Spectrum `activeRange` restores the
highlighted range. Register readers accept only the canonical mode string;
writes omit the field when no mode was recorded.

Terminology used throughout:

| Term | Meaning |
|---|---|
| **register** / **slot** | One stored `{frequency, mode}` position, IC-705 "Band Stacking Register" |
| **stack** | The ordered set of registers belonging to one band |
| **bucket** | A proposed *ownership* neighbourhood of spectrum with a stable id — what a stack is keyed by |
| **plan band** | An entry in `root/res/bandplans/*.json` — a display/navigation artefact |
| **shadow** | The live, uncommitted "where we are now", Thetis's `bsf.LastVisited` |
| **commit** | Writing the shadow into a register |
| **recall** | Tuning (and setting mode) from a register |

---

## Part 1 — The current implementation

### 1.1 Architectural placement

The band stack is *application state*: it has to observe tuning that originates
anywhere (digit widget, mouse wheel, waterfall drag, VFO drag, keypad,
bookmarks, rigctl, QMX rig knob) and it has to be durable across a process that
may be killed without notice. So it is not part of the widget that draws it:

| Responsibility | Lives in |
|---|---|
| Digit widget | `gui/widgets/frequency_select.{h,cpp}` |
| F-INP keypad page | `gui/widgets/freq_input/keypad.cpp` |
| Band grid page | `gui/widgets/freq_input/bands.cpp` |
| Modal shell, page toggle | `gui/widgets/freq_input/dialog.cpp` |
| Band stack store | `gui/widgets/band_stack.{h,cpp}`, instance `gui::bandStack` |
| Persisted layout, lifecycle commit | `gui/widgets/freq_memory.{h,cpp}` |

The dialog's three parts share one module header, `gui/widgets/freq_input.h`,
which defines what a page is handed (`Context`, `Metrics`) and what it returns
(`Outcome`). Routing the keypad's result back through `Outcome` leaves
`keypad.cpp` with no `gui::`, `core::` or `config.h` dependency at all, so the
one page that is pure input handling is testable without an application around
it.

`gui::bandStack` is reachable from anything on the UI thread. Its callers are
the band grid (tap, long-press, register pick) and `freq_memory::commitCurrent()`,
which `core.cpp` runs at shutdown and from the lifecycle hook. Nothing polls the
tuning between those events (Part 3 proposes that); §8.8 lists where each
remaining P1 item drops in.

### 1.2 Keying: stable band IDs, not plan names

Registers are keyed by the stable band ID of the canonical `BandMapping` a plan
row resolves to (`gui/widgets/band_mapping.{h,cpp}`), not by the plan band's
name. Plan band names are neither unique within a plan nor stable across plans.
Seventeen of the 21 files in `root/res/bandplans/*.json` reuse at least one band
name; only ireland, slovakia and the two germany-mobile-* sets do not. The
thirteen most affected:

| Plan | Bands | Names used more than once | Worst |
|---|---|---|---|
| netherlands.json | 326 | 52 | `Maritime Mobile Service` ×9 |
| italy.json | 197 | 29 | `Mobile marittimo` ×15 |
| russia.json | 176 | 16 | `L-Band` ×6, `S-Band` ×6 |
| brazil.json | 106 | 14 | `CW, Digital` ×9 |
| republic-of-korea.json | 90 | 14 | `Amateur Station` ×12 |
| turkey.json | 46 | 6 | `70cm` ×4 |
| united-kingdom.json | 104 | 5 | `Aeronautical Mobile` ×13 |
| australia.json | 61 | 5 | `Shortwave Broadcast` ×15 |
| france.json | 85 | 4 | `Marine - HF` ×14 |
| china.json | 46 | 3 | `Shortwave Broadcast` ×15 |
| **general.json** (default) | 44 | 2 | `Shortwave Broadcast` ×15 |
| usa.json | 57 | 2 | `Shortwave Broadcast` ×14 |
| germany.json | 76 | 2 | `Aeronautical HF` ×12 |

A small count is not a small problem: general.json has just two duplicated
names, and one of them covers all fifteen shortwave broadcast segments, which
a name-keyed store would collapse into one three-slot stack. Names are also
region-specific (`40m Ham Band` / `40m - Radioamateur` / `Radioamatori 80m`),
so a name-keyed store would orphan every stack on a plan switch. Stable IDs
avoid both.

Plans also overlap: in general.json, `70cm Ham Band` (420–450 MHz) wholly
contains `PMR446` (446.0–446.2 MHz), and differently-named overlapping pairs
exist in 9 of the 21 shipped plans. Write-back therefore resolves the band to
store under through the visible service: `BandStack::resolveBandForServices()`
lets the preferred service win when it contains the frequency, and otherwise
takes the first match in another visible service.

### 1.3 The current register

Entry 0 of a band's three is its current register, so no separate pointer is
stored: ordinary write-back overwrites entry 0 in place, and only an explicit
band or register action rotates the array (the rules are under **What is
built** above, and in `BandRegisterSet`). This matches Icom ("when you change
the operating band or the Register, the previously operated frequency and mode
are stored") and Thetis (`UpdateCurrentWithLastVisitedData` overwrites in
place, never pushes): continuous tuning on one band consumes no slots, and a
repeat tap cycles the stack, as `BN$^;` does on the K4, `Next()` in Thetis and
the band key on Icom and Yaesu (note §5 gap 4).

Opening the active band's register list is a persistence checkpoint: it stores
the current VFO in entry 0 and then shows a frozen snapshot
(`BandRegisterPopupSnapshot`), so later radio changes do not rewrite the list
under the user. The popup holds the static canonical `BandMapping`, not a
plan-owned row, so a plan reload cannot invalidate it. If the selected VFO's
mode already matches the register's, `SET_MODE` is skipped, because the radio's
`selectDemodByID()` has no early-out and rebuilds the demodulator chain, which
is audible.

### 1.4 Open findings, ordered by severity

1. **No GEN bucket** (note gap 2). Write-back stores nothing when no band
   contains the frequency, so a shortwave or WWV excursion outside the plan is
   dropped. Icom has code 15 `GENE` — literally "other than above"; Thetis has
   `Band.GEN` *and* `waterfall_low_threshold_gen`.
2. **`band.chan` snap bypasses the radio module.** This is one confirmed
   instance of the systematic ownership problem tracked in
   `doc/bugs/state-ownership-bypass.md`. `BandStack::applySegmentTarget()`
   calls `setSnapInterval(segment.chan)` directly on the waterfall VFO while
   `RadioModule::snapInterval` keeps its own value. Two consequences: the radio
   menu's snap field disagrees with what the VFO actually does; and the override
   survives only until the next `SET_MODE`, so on a band with no mode
   (`targetMode < 0`, e.g. any `utility`-type band) the channel snap **leaks
   into the next band**. The honest fix is an append-only
   `RADIO_IFACE_CMD_SET_BAND_SNAP_INTERVAL` with explicit apply/clear semantics
   so the module stays authoritative without overwriting the per-mode setting.
3. **`mode = -1` conflates "no mode" with "no radio module".**
   `BandStack::currentMode()` returns −1 when the selected VFO is not a radio,
   and that −1 is stored. Coming back to the band then applies the *heuristic*
   mode rather than restoring anything, and the register has no way to express
   "leave the mode alone".
4. **Free tuning is committed only at checkpoints.** The register is written
   on a band transition, on opening the register list, and at shutdown or the
   lifecycle hook. Nothing polls in between, so a crash after an evening of
   free tuning loses it. `freq_memory::commitCurrent()` is the commit; Part 3's
   poll is what is missing.
5. **No lock, no label, no delete, no "store to slot k".** With automatic
   write-back into three slots, a lock is what keeps a curated entry (the FT8
   frequency) from being consumed. Thetis's rule is one line: skip the commit if
   `Locked`, but always apply the lock flag itself so it stays unlockable.
6. **The mode convention is duplicated across a language boundary** —
   `BandStack::heuristicMode()` and `heuristic_mode()` in
   `scripts/enrich_bandplans.py`, with no shared source of truth; both sides
   carry a "keep in sync" comment. (`qmx_source`'s `FreqModeSync` maps
   `qmx::QmxMode` ↔ `RADIO_IFACE_MODE_*`, an enum translation for one rig, not
   the band→mode convention.)
7. **Partial-overlap recall can exceed the source range.** A band that only
   partially overlaps `[minFreq, maxFreq]` survives the grid filter
   (`freq_input/bands.cpp`), so its midpoint default or a stored register can
   land outside the range and be silently clamped by `draw()` on the next
   frame.

### 1.5 What is right and must be preserved

- **The layering.** Layer 2 (per-demod profiles: bandwidth, snapInterval,
  squelch, CTCSS, high-pass, de-emphasis, FM IF NR, noise blanker — stored at
  `config.conf[<vfo>][<demod>]` by the radio module) stays out of the register.
  That matches Icom, where the filter preset is keyed by mode rather than by
  stack slot. The note's "deliberately not recommended" list holds: no per-entry
  bandwidth, no second stack for a second VFO.
- **Reading the tune frequency**, `FrequencySelect::frequency` — the selected
  VFO's display-domain frequency — rather than the hardware centre. Correct, and
  the same choice Thetis makes (`BandStackEntry.frequency` is the VFO
  frequency).
- **The resolution order** memory → `def_freq` → band midpoint, and mode from
  memory → `def_mode` → convention. `BandStack::heuristicMode()` encodes the
  same (band × mode-family) → exact-variant matrix the K4 exposes as `MA$`,
  including the documented "LSB for 160/80/40 m, USB above" convention.
- **Range-filtering the grid** by `limitFreq`/`minFreq`/`maxFreq` in the display
  domain, and the tap-vs-long-press gesture split with a motion slop test.
- **The register list popup** as the long-press surface — it just needs actions.

---

## Part 2 — Proposed data model

Two decisions carry the whole design:

> **D1.** Ownership is keyed by a code-defined spectrum partition (**buckets**),
> not by band-plan bands.
>
> **D2.** The shadow is maintained by **polling** once per frame, not by hooking
> the tune path.

### 2.1 D1 — separate "what the grid shows" from "what owns the memory"

These are different concepts:

- **Plan bands** are a *display and navigation* artefact: region-specific,
  overlapping, duplicate-named, sometimes 326 of them, sometimes 9 segments of
  one ham band. Exactly right for drawing the grid and the spectrum ruler.
  Categorically wrong as a persistence key.
- **Buckets** are an *ownership* partition: a small, ordered, total,
  code-defined list of spectrum neighbourhoods with stable string ids, ending in
  a `GEN` catch-all. This is what stacks and per-band display state are keyed by.

This is precisely the structure both primary sources use. Icom's band codes are
neighbourhoods deliberately *wider* than the legal allocation (code 02, "3.5",
owns 3.400000–4.099999) with an explicit `GENE` for everything else; Thetis's
`FRSRegion.Extended` tiles the entire spectrum with no gaps and adds
`Band.GEN`. Keying on buckets fixes, in one move: duplicate names, overlap
ambiguity, plan switching, the missing GEN bucket, *and* the absurdity of
per-plan-segment noise-floor calibration in a 326-band plan.

**Authoring rule.** An ordered list; **first bucket whose any range contains the
frequency wins**; the final entry is `GEN` with an infinite range. Overlaps in
the table are therefore legal and are how specificity is expressed (630 m before
NDB, 13 cm before 2.4 GHz Wi-Fi) — the same trick Icom's table uses implicitly
by giving ham bands the wide neighbourhoods. A bucket may own **several ranges**,
which is needed for inherently segmented services (aeronautical HF is 13
segments in france.json, maritime HF 14).

### 2.2 The bucket table

Concrete proposal, 51 entries including the `GEN` catch-all. Ham neighbourhoods
follow Icom's widths, trimmed
only where they would swallow an ITU HFBC broadcast band. The shortwave buckets
are built on the ITU HFBC segments — which general.json's fifteen
`Shortwave Broadcast` entries reproduce exactly — but are **rounded outward**
from them, so a bucket is generally a little wider than the segment it is named
for (`SW49m` is 5.800–6.300 for an HFBC segment of 5.900–6.200). That is
deliberate, and the same principle as Icom's ham neighbourhoods: a bucket is an
ownership catchment, not an allocation. Ordered as listed.

| id | label | ranges (MHz) | provenance |
|---|---|---|---|
| `2200m` | 2200 m | 0.130 – 0.140 | |
| `630m` | 630 m | 0.470 – 0.480 | |
| `LW` | LW | 0.140 – 0.290 | LW broadcast 148.5–283.5 kHz |
| `NDB` | NDB | 0.290 – 0.470, 0.480 – 0.5265 | multi-range, carved around 630 m |
| `MW` | MW | 0.5265 – 1.710 | |
| `160m` | 160 m | 1.800 – 2.000 | Icom 01 |
| `SW120m` | 120 m | 2.300 – 2.500 | HFBC |
| `SW90m` | 90 m | 3.200 – 3.400 | HFBC |
| `80m` | 80 m | 3.400 – 3.950 | Icom 02, trimmed below 75 m |
| `SW75m` | 75 m | 3.950 – 4.100 | HFBC + Icom 02 upper edge |
| `SW60m` | 60 m SW | 4.700 – 5.100 | HFBC (two segments, one bucket) |
| `60m` | 60 m | 5.250 – 5.500 | GENE on the IC-705; note §1 |
| `SW49m` | 49 m | 5.800 – 6.300 | HFBC |
| `40m` | 40 m | 6.900 – 7.200 | Icom 03, trimmed below 41 m |
| `SW41m` | 41 m | 7.200 – 7.500 | HFBC + Icom 03 upper edge |
| `SW31m` | 31 m | 9.300 – 9.900 | HFBC |
| `30m` | 30 m | 9.900 – 10.500 | Icom 04 |
| `SW25m` | 25 m | 11.500 – 12.200 | HFBC |
| `SW22m` | 22 m | 13.500 – 13.900 | HFBC |
| `20m` | 20 m | 13.900 – 14.500 | Icom 05 |
| `SW19m` | 19 m | 15.100 – 15.800 | HFBC |
| `SW16m` | 16 m | 17.480 – 17.900 | HFBC |
| `17m` | 17 m | 17.900 – 18.500 | Icom 06 |
| `SW15m` | 15 m SW | 18.900 – 19.020 | HFBC |
| `15m` | 15 m | 20.900 – 21.450 | Icom 07, trimmed below 13 m |
| `SW13m` | 13 m | 21.450 – 21.850 | HFBC |
| `12m` | 12 m | 24.400 – 25.100 | Icom 08 |
| `SW11m` | 11 m | 25.600 – 26.100 | HFBC |
| `CB` | CB | 26.500 – 27.500 | |
| `10m` | 10 m | 28.000 – 30.000 | Icom 09 |
| `6m` | 6 m | 50 – 54 | Icom 10 |
| `4m` | 4 m | 70 – 70.5 | |
| `WFM` | FM bcast | 74.8 – 108 | Icom 11 |
| `AIRVHF` | Air | 108 – 137 | Icom 12 |
| `VHFSAT` | VHF sat/wx | 137 – 144 | |
| `2m` | 2 m | 144 – 148 | Icom 13 |
| `VHFLO` | VHF low | 148 – 156 | |
| `MARVHF` | Marine | 156 – 163 | |
| `VHFHI` | VHF high | 163 – 174 | |
| `125cm` | 1.25 m | 219 – 225 | before `DAB`, which contains it |
| `DAB` | DAB / VHF TV | 174 – 240 | |
| `AIRUHF` | Mil air | 225 – 400 | |
| `70cm` | 70 cm | 420 – 450 | Icom 14 |
| `UHFPMR` | UHF PMR | 400 – 420, 450 – 470 | |
| `UHFTV` | UHF TV | 470 – 790 | |
| `CELL` | Cellular | 790 – 960 | |
| `23cm` | 23 cm | 1240 – 1300 | |
| `LBAND` | L band | 960 – 1240, 1300 – 1700 | ADS-B, GPS, Inmarsat, wx sat |
| `13cm` | 13 cm | 2300 – 2450 | before `ISM24` |
| `ISM24` | 2.4 GHz | 2400 – 2500 | |
| `GEN` | General | everything else | Icom 15, `GENE` |

Notes on the choices:

- **Region-independence is a feature, not a shortcut.** Ownership answers "where
  was I", not "what is legal here". Icom's own table is region-independent, and
  keeping ours so is what makes stacks survive a band-plan switch.
- **Keep the table in C++, not JSON.** User edits would orphan stacks and break
  the migration, which depends on bucket ranges being stable. If regional
  extension is ever wanted, add an additive `bandStack.extraBuckets` later.
- **The trimmed ham/HFBC boundaries are the arguable part.** Everything else is
  disjoint after trimming, so the ordering is load-bearing for exactly three
  overlapping pairs — checked mechanically against the table as written:

  | Earlier bucket | Later bucket | Contested span |
  |---|---|---|
  | `125cm` 219–225 | `DAB` 174–240 | 219–225 (nested) |
  | `DAB` 174–240 | `AIRUHF` 225–400 | 225–240 (partial) |
  | `13cm` 2300–2450 | `ISM24` 2400–2500 | 2400–2450 (partial) |

  `630m`/`NDB` is *not* one of them: `NDB` is written as two ranges precisely so
  that it excludes 0.470–0.480, which makes the pair disjoint and the ordering
  between them irrelevant. `DAB`/`AIRUHF` takes its place — 225–240 MHz is
  claimed by both, and as ordered it goes to `DAB`. That is the right answer for
  VHF TV band III, but it is a decision the table currently makes silently.
- `60m` (5.250–5.500) has no Icom code — the IC-705 puts 5 MHz in GENE. Given
  60 m is a real allocation now, it gets a bucket.
- **The table has no aeronautical-HF or maritime-HF bucket**, which is a gap
  against §2.1's own reasoning: the justification given there for letting a
  bucket own several ranges is "inherently segmented services (aeronautical HF
  is 13 segments in france.json, maritime HF 14)" — both counts verified — and
  then neither service appears. As drawn, all thirteen `Aviation - HF` segments
  and all fourteen `Marine - HF` segments land in `GEN`. The multi-range
  machinery exists and only `NDB`, `UHFPMR` and `LBAND` use it. See open
  question 6.
- **`GEN` is doing more work below 30 MHz than the table implies.** The HF
  buckets leave nineteen gaps under 30 MHz: everything below 130 kHz, then
  1.71–1.80, 2.00–2.30, 2.50–3.20, 4.10–4.70, 5.10–5.25, 5.50–5.80, 6.30–6.90,
  7.50–9.30, 10.50–11.50, 12.20–13.50, 14.50–15.10, 15.80–17.48, 18.50–18.90,
  19.02–20.90, 21.85–24.40, 25.10–25.60, 26.10–26.50 and 27.50–28.00 MHz. That
  is where HF utility listening lives, and all of it shares one three-slot
  stack. Icom does the same with `GENE`, so this is precedent-backed rather than
  wrong — but for a fork whose stated shortwave rationale (open question 1) is a
  KiwiSDR source and an EIBI module, it is worth deciding knowingly rather than
  inheriting.

Plan bands map to buckets by **the bucket containing the plan band's centre**,
which is single-valued and therefore never ambiguous.

### 2.3 The model

```cpp
// One stacking register.
//
// Deliberately ABSENT, and why:
//   bandwidth / filter  -- owned by the per-demod profiles (research §5); a copy
//                          here would fight config.conf[<vfo>][<demod>].
//   hardware centre     -- in SDRIAK the centre is derived from the tune
//                          frequency by tuner::tune() per tuning mode, so
//                          storing it (as Thetis does for its panadapter)
//                          would fight the tuner.
//   zoom / view offset  -- display state, so it belongs in BandState below,
//                          not in a register (research §3).
struct BandRegister {
    double      freq   = 0;      // VFO tune frequency, Hz, display domain
    int         mode   = -1;     // RADIO_IFACE_MODE_*; -1 = leave the mode alone
    bool        locked = false;  // never overwritten or evicted by write-back
    std::string label;           // optional user label ("FT8", "county net")
};

// Everything remembered about one bucket.
struct BandState {
    // Exactly three optional entries. The array rotates only on an explicit
    // band/register action, so regs[0] is always current without a pointer.
    std::array<std::optional<BandRegister>, 3> regs;

    // Layer 3: per-band display calibration. Absent => inherit the global
    // min/max. It lives HERE and not in a register because the noise floor on
    // 80 m is a property of 80 m, not of the third register on 80 m -- the
    // split Thetis uses (flat per-band waterfall_*_threshold_<band> map,
    // separate from BandStackEntry). See research §3.
    bool  haveDisplay = false;
    float fftMin = 0, fftMax = 0;
    float zoom   = -1;                // view-bandwidth slider, -1 = absent
};

// The live, uncommitted "where we are now" -- Thetis's bsf.LastVisited.
struct Shadow {
    const Bucket* band  = nullptr;
    double        freq  = 0;
    bool          valid = false;
    double        dwell = 0;          // seconds since freq last changed
};
```

Service state: `std::map<std::string, BandState> bands;  Shadow shadow;
bool perBandDisplay = false;`

**The shadow deliberately carries no mode.** The mode is read from the radio at
commit time via `RADIO_IFACE_CMD_GET_MODE`. This removes any need for a
mode-change event — the radio module offers none, and adding one would mean new
module ABI — and removes any way for the shadow to go stale against the radio.

### 2.4 Invariants

- `regs.size() == 3`; each index is a fixed physical position and may be empty.
  `regs[0]` is current whenever populated. If any entry is empty, a repeat
  rotates the prefix ending at the first empty entry right, then stores the
  current state at `regs[0]`: `[A, B, empty] + X -> [X, A, B]`,
  `[A, empty, C] + X -> [X, A, C]`, and
  `[empty, B, C] + X -> [X, B, C]`. Once full, a repeat overwrites `regs[0]`
  and rotates all three positions right: `[A, B, C] + X -> [C, X, B]`.
  A selected empty popup row is materialized without overwriting the old top,
  then promoted by rotating only the prefix through that row.
- Every populated `regs[i].freq` lies inside the bucket's ranges. Revalidated on load —
  and because bucket ranges are code-defined and stable, revalidation almost
  never discards anything.
- Locked entries are never overwritten or evicted. **If every entry in a stack is
  locked, commit is a no-op** — state this explicitly or the eviction search has
  no terminating case.
- Entries are **not MRU-ordered**. Ordinary write-back changes only `regs[0]`;
  explicit repeat-tap or row selection performs a cyclic rotation.

### 2.5 Config schema

```jsonc
"bandStack": {
  "version": 2,
  "perBandDisplay": false,
  "bands": {
    "40m":   { "regs": [ { "f": 7030000, "m": 5, "lock": true,  "label": "CW"  },
                         { "f": 7074000, "m": 4, "lock": false, "label": "FT8" },
                         null ],
               "display": { "min": -92.0, "max": -20.0, "zoom": 0.35 } },
    "SW31m": { "regs": [ /* exactly three entries/nulls; index 0 current */ ] },
    "GEN":   { "regs": [ /* ... */ ] }
  }
}
```

`m` is a snapshot of `RADIO_IFACE_MODE_*` (NFM 0, WFM 1, AM 2, DSB 3, USB 4,
CW 5, LSB 6, RAW 7, CWR 8), so changing a plan default does not reinterpret a
stored register. An unavailable value of −1 defers to the containing
segment's current default/convention at recall. `display` absent means inherit
the global `min`/`max`.
Bucket ids are stable, human-readable, and independent of the selected plan.

### 2.6 Migration — and a constraint that dictates where it runs

This migration belongs to the service-owned-catalog release. At that
boundary:

1. For each `frequencyMemory.band.stackingRegisters[<band ID>]`, map each
   populated entry to the service-owned band containing it while preserving
   cyclic array order.
2. Keep the source top entry at destination index 0; pad missing entries with
   `null`; set `haveDisplay = false`.
3. Write `bandStack` with `version: 2`; erase `stackingRegisters`.

Lossless for every stored entry.

**Where it must run.** `core/src/core.cpp` repairs the config by **erasing every
top-level key that is not in `defConfig`**. That runs immediately after
`configManager.load(defConfig)` and *before* the existing migration block
(`style::migrateLogicalDimension`). So if the stacking registers' key is
dropped from `defConfig` in the same commit that adds `bandStack`, the repair
pass deletes the old data before any code in `gui::mainWindow.init()` could
migrate it.

Therefore: the migration call must be placed **between `load()` and the
unused-key repair loop** — a free function
`bandstack::migrateConfig(core::configManager.conf)`, following the precedent of
`style::migrateLogicalDimension`. The alternative (keep the old key in
`defConfig` for one release) leaves dead config keys around and needs a
follow-up commit; not worth it.

Downgrade behaviour is worth stating: an older build reading a `version: 2`
config finds no stacking registers, sees `bandStack` as an unused key, and
**erases it**. Downgrading loses band memory.

---

## Part 3 — Proposed state machine

### 3.1 D2 — poll, don't hook

`BandStack::update(double deltaTime)` runs once per frame from
`MainWindow::draw()`. It reads the selected VFO's tune frequency and derives
everything else. It does **not** hook `tuner::tune()` or any tuning call site.

Three reasons, in order of importance:

1. **Thread safety.** `tuner::tune()` is called from the rigctl network thread —
   the known, *postponed* bug in `doc/bugs/ui-thread-sync.md` (issue #1437). Hooking the tune path would put band-stack mutation and config writes
   on that thread. Polling from the draw loop keeps everything on the UI thread
   by construction, and does not depend on #1437 ever being fixed.
2. **Coverage.** One function catches digit steps, the mouse wheel, keypad ENT,
   waterfall drag, VFO drag, frequency-manager bookmarks, rigctl, and the QMX
   `FreqModeSync` rig-knob path — including future callers that would never know
   to notify us. Turning the band up on a QMX front panel gives us the band
   stack for free.
3. **Testability and reasoning.** The machine reduces to a pure transition
   `step(freq, mode, dt) → actions`, which `core/tests` can cover the way
   `band_register_set_tests` covers the register rotation.

VFO switching falls out for free: selecting another VFO changes the polled
frequency and is handled by the ordinary bucket-transition rule. As in Thetis —
which is multi-receiver and still restricts stack tracking to RX1 — there is one
stack, following the *selected* VFO.

### 3.2 Machine A — shadow tracking (continuous, once per frame)

States: `Idle` (no valid shadow) and `Tracking(bucket)`.

```
update(dt):
    f = currentTuneFrequency()              // selected VFO, display domain
    b = bucketOf(f)                         // always succeeds; GEN is total

    if !shadow.valid:                       // Idle -> Tracking (startup)
        shadow = { b, f, valid, dwell = 0 }
        bands[b].current = clamp(bands[b].current)
        applyDisplayState(b)
        return                              // no commit, no recall

    if b == shadow.band:
        if f != shadow.freq: shadow.freq = f; shadow.dwell = 0
        else:                shadow.dwell += dt
        maybeAutoCommit()                   // see 3.4
        return

    // --- bucket crossing by free tuning ---
    // Thetis's OnBandChangeHandler: does NOT commit and does NOT recall. It
    // re-points LastVisited.Band, calls SelectInitial() on the new band and
    // leaves the frequency alone.
    shadow.band = b; shadow.freq = f; shadow.dwell = 0
    applyDisplayState(b)                    // but DO recall display state
```

The asymmetry in the last two lines is the subtle heart of the design:

> **Band keys recall tuning state; frequency changes only classify.**

Committing on a crossing would overwrite the register of a band you merely tuned
*past*. Recalling on a crossing would fight the user's own tuning — you could
never tune across 30 m. But *display* state is a property of where the receiver
is pointing right now, so it does follow the crossing; that is what makes
per-band waterfall calibration feel automatic rather than modal.
`applyDisplayState()` is a no-op while `perBandDisplay` is off or
`autoRange.sticky()` is latched.

### 3.3 Machine B — commit and recall

```
commit():
    if !shadow.valid: return
    S = bands[shadow.band]
    e = &S.regs[0]                                      // top is always current
    if e->locked: return                                  // Thetis rule
    if e->freq == shadow.freq && e->mode == currentRadioMode(): return   // no churn
    e->freq = shadow.freq
    e->mode = currentRadioMode()                          // -1 when not a radio
    saveConfig()
    // NEVER pushes. Continuous tuning consumes zero slots.
```

```
recall(bucket b, int k, const Band_t* tapped):
    S = bands[b]
    if k >= 0: rotate_right(S.regs[0..clamp(k)])          // picked entry becomes 0
    target = S.regs[0].has_value() ? *S.regs[0] : defaultFor(b, tapped)
    shadow = { b, target.freq, valid, dwell = 0 }          // suppress machine A
    applyDisplayState(b)
    if target.mode >= 0 && isRadio && target.mode != currentRadioMode():
        SET_MODE(target.mode)                              // guard: SET_MODE rebuilds the demod
    applySnap(tapped)                                      // see 3.6
    tune(target.freq)                                      // immediate NORMAL tune on the UI thread
    saveConfig()
```

`defaultFor(b, tapped)`: `tapped->defFreq` if set; else `tapped`'s midpoint
rounded to 1 kHz; else the bucket's first range midpoint. Mode from
`tapped->defMode`, else `BandStack::heuristicMode(*tapped)`, else the bucket's
own default mode.

### 3.4 Commit and recall triggers

| Trigger | Action |
|---|---|
| Band key tapped, bucket **≠** shadow bucket | Validate and overwrite the visible source's top entry, then recall the target's top entry |
| Band key tapped, bucket **==** shadow bucket (repeat tap) | If an empty entry exists, rotate the prefix through the first empty entry right and then store the current state at the top. If full, overwrite the top and rotate all three entries right. Recall the resulting top |
| Long-press band key | For the visibly active band, overwrite and persist the top register from the current in-band VFO state. Otherwise seed only an empty top from the band default. Snapshot the resulting list and keep it frozen while the popup is open; never tune or rotate |
| Register *k* picked from the list | Row 0 recalls the frozen value shown. For populated rows 1 and 2, overwrite the visible source with its state at selection time, rotate prefix `[0, k]` right once, and recall the promoted value. If row *k* is empty, materialize it from an in-band VFO without first overwriting the target's old top, rotate the prefix, and recall it; otherwise keep the popup open |
| Lock / label / delete a slot | Mutate the slot. The lock flag itself is always settable, even on a locked entry, so it stays unlockable |
| Frequency-input Band page opened or group changed | Resolve visibly inside that group and activate/scroll. Write no register; selector ownership and a fallback service may be persisted |
| Application shutdown | Resolve inside the last group **and current service only**, then overwrite the top without rotating |
| Android `APP_CMD_PAUSE` | `commit()` — see 3.7 |

**No dwell write-back.** Opening the active band's register list is an explicit
persistence checkpoint: it captures the current state into the top before
taking the frozen popup snapshot. Other registers change only on an explicit
band/register transition or at an application persistence boundary. Android
pause/stop remains such a boundary because the process may be killed without a
later destruction callback. Its save resolver is restricted to the last group
and current service, so it can follow manual tuning from 20 m to 40 m but cannot
silently reclassify Amateur as Broadcast.

### 3.5 Recall filtering — tapping a segment of a bucket

Buckets are coarser than plan bands, so a plan band can be a strict subset of
its bucket. brazil.json has nine `CW, Digital` segments inside single ham bands;
general.json has `PMR446` inside `70cm`. If the user taps a narrow CW segment
while the bucket's current register holds an SSB frequency, jumping to the SSB
frequency is wrong — the tap expressed an intent.

So `recall` takes the tapped plan band as a **filter**:

```
selectFromKey(tapped):
    b = bucketOfCentre(tapped)
    if tapped covers the whole bucket:
        k = bands[b].current                  // plain register semantics
    else:
        // first matching slot at or after current, wrapping; -1 if none
        k = findSlotInRange(b, tapped->start, tapped->end, from = bands[b].current)
    recall(b, k, tapped)                      // k = -1 -> defaultFor(tapped)
```

Repeat-tap cycling then cycles over the *matching* subset. This is exactly
Thetis's `BandStackFilter` + `FindForFrequencyRange` mechanism, which the
research note describes as filtering on band / mode / sub-mode / frequency
range — the same problem, the same answer.

### 3.6 Mode and snap application

- **Mode:** always compare against `GET_MODE` before `SET_MODE`, because
  `selectDemodByID()` has no early-out and rebuilds the demod chain
  unconditionally (§1.3). `mode == -1` means leave the mode alone;
  distinguish it from "no radio module selected" by checking
  `modComManager.getModuleName(vfo) == "radio"` separately.
- **Snap:** route `chan` through a new append-only
  `RADIO_IFACE_CMD_SET_SNAP_INTERVAL` so `RadioModule::snapInterval` and its
  menu field stay authoritative. Recall order stays mode-then-snap, since
  `SET_MODE` reloads the demod's snap from config. When recalling into a band
  with no `chan`, explicitly re-apply the demod's stored snap so a channelized
  band cannot leak its 25 kHz grid into the next band (finding §1.4/2).
- **Range clamping:** clamp the recall target into `[minFreq, maxFreq]` before
  tuning when `limitFreq`, so a partially-in-range band cannot produce a
  one-frame excursion (finding §1.4/7).

### 3.7 Edge cases

| Situation | Behaviour |
|---|---|
| No VFO selected | Poll `waterfall.getCenterFrequency()`; mode is −1; commit stores −1 |
| Selected VFO is not a radio | Frequency tracked normally; mode −1; recall skips `SET_MODE` |
| Selected VFO changes | Polled frequency changes → ordinary bucket-crossing rule; no commit, no recall |
| Source tuning range changes mid-session | Grid re-filters; stored registers untouched (they are keyed by bucket, not by range); recall clamps |
| No band plan loaded | Grid shows the existing "No band plan loaded"; the service still tracks and commits — buckets do not need a plan |
| Band plan switched | Stacks survive (this is the point of D1). Grid re-derives; the register popup holds the static canonical mapping and a value snapshot, so it cannot dangle |
| Sticky auto-range latched | `applyDisplayState` suppressed; optionally *capture* the converged level into the bucket (see §4) |
| Server (headless) mode | `core.cpp:438` takes the `server::main()` branch and never reaches `gui::mainWindow.init()`; the service is simply never constructed. No guard needed, but do not reference it from `signal_path` code, which *does* run headless |
| All slots locked | Commit is a no-op; the register list shows the locks so this is visible rather than mysterious |
| Frequency 0 / source stopped | `bucketOf(0)` → `GEN`; suppress tracking while `!isPlaying()` to avoid parking a `GEN` register at 0 |

---

## Part 4 — Layer 3: per-band display state

The waterfall range is the two global scalars `conf["min"]` / `conf["max"]`
(`main_window.cpp`). Thetis has stored
`display_grid_min_<band>` and `waterfall_low/high_threshold_<band>` per band,
per receiver, with `_gen` / `_wwv` / `_xvtr` buckets, for over a decade
(research §3), and the flat per-band map — separate from the stack entry — is
the architecture to copy.

- **Storage:** `BandState.{haveDisplay, fftMin, fftMax, zoom}`.
- **Applied** by `applyDisplayState(b)` on every bucket entry — band-key recall
  *and* free-tuning crossing — skipped while `perBandDisplay` is off or
  `autoRange.sticky()` is latched. Buckets without calibration inherit the
  global `min`/`max`, so behaviour is unchanged until the user calibrates.
- **Captured** when the user moves the Ref or Range slider: the handlers in
  `drawWaterfallControls()` (`main_window.cpp`) already write
  `conf["min"]/["max"]` and would additionally write
  `bands[shadow.band].display`.
- **Calibrated for free by sticky auto-range.** While the latch is on,
  periodically capture the converged Ref/Range into the current bucket. The
  auto-scaler then doubles as a per-band calibrator, so unlatching it leaves a
  sensible stored level for every band the user has visited. This is a small
  addition and it is what makes the feature pay for itself with no manual work —
  and it resolves the interaction the research note flags as open ("when
  auto-range is latched the stored levels are redundant, so restore them only
  when it is off").
- **`zoom`** (the view-bandwidth slider) is the one piece of Thetis's
  `BandStackEntry` display state worth adopting, and it belongs here rather than
  in a register. Optional; behind the same toggle.
- **Off by default** (`bandStack.perBandDisplay = false`) with a display-menu
  toggle. It visibly changes waterfall behaviour, so it must be opt-in.
- `GEN` gets its own calibration, like Thetis's `waterfall_low_threshold_gen`.

---

## Part 5 — Integration

### 5.1 Files

**New — `core/src/gui/widgets/band_stack.{h,cpp}`.** The bucket table, `bucketOf()`, the
store with load/save/migrate, both machines, `commit`/`recall`, display state,
the mode convention, and the mode/snap application helpers. One instance
exported from `gui.h` alongside `gui::freqSelect`. Header carries an explicit
threading contract in the style of
`source_modules/qmx_source/src/FreqModeSync.h`:

```cpp
// Threading contract:
//   - Every public method is UI-thread only, called from MainWindow::draw()
//     or from FrequencySelect's modal (which draws on the same thread).
//   - Nothing here is called from the tune path, so rigctl's network thread
//     (see doc/bugs/ui-thread-sync.md) never touches this state. Frequency is
//     sampled, not pushed.
//   - migrateConfig() is a free function, called once from core.cpp before the
//     config-repair pass and before any thread exists.
```

Sketch of the public surface:

```cpp
namespace bandstack {
    struct Range   { double lo, hi; };
    struct Bucket  { const char* id; const char* label;
                     const Range* ranges; int nRanges; int defMode; };

    const Bucket*  bucketOf(double hz);                  // never null; GEN is total
    const Bucket*  bucketOfCentre(const bandplan::Band_t& b);
    int            heuristicMode(const bandplan::Band_t& b);
    void           migrateConfig(json& conf);            // called from core.cpp
}

class BandStack {
public:
    void init();                       // load from config
    void update(double deltaTime);     // machines A + the dwell timer
    void commit();                     // explicit: dialog open, shutdown, pause

    // Called by the band grid.
    void selectFromKey(const bandplan::Band_t& tapped);          // tap
    void recallSlot(const bandplan::Band_t& tapped, int slot);    // list pick
    void storeToSlot(const bandplan::Band_t& tapped, int slot);
    void addSlot(const bandplan::Band_t& tapped);
    void setLocked(const std::string& bucket, int slot, bool on);
    void setLabel(const std::string& bucket, int slot, const std::string& s);
    void eraseSlot(const std::string& bucket, int slot);

    // Read-only views for the UI.
    const BandState* stateFor(const bandplan::Band_t& tapped) const;
    std::vector<const BandRegister*> registersInSpan(double lo, double hi) const;

    // Layer 3.
    void captureDisplay(float fftMin, float fftMax);
    bool perBandDisplayEnabled() const;
};
```

**The band grid is a view.** `gui/widgets/freq_input/bands.cpp` draws keys
and register lists and calls `BandStack` (Part 8). For P1 its popup identifies
a bucket id + slot index, and its two calls become `selectFromKey` /
`recallSlot`.

**`main_window.cpp`** — `bandStack.update(deltaTime)` once per frame, next to
`autoRange.update()`; the Ref/Range slider handlers also call
`captureDisplay()`.

**`core.cpp`** — `defConfig["bandStack"]`, and `bandstack::migrateConfig()`
between `load()` and the unused-key repair (§2.6). The shutdown and lifecycle
commits already run through `freq_memory::commitCurrent()`.

**Android** has no shutdown path (`core.cpp`, `#ifndef __ANDROID__`), so the
lifecycle commit plus the dwell timer are the whole durability story there.

**`radio_interface.h` / `radio_module.h`** — append
`RADIO_IFACE_CMD_SET_SNAP_INTERVAL` (append-only keeps existing numeric values
stable, as the Brown AF-chain commands already do) and handle it by setting
`snapInterval`, calling `vfo->setSnapInterval()` and saving to
`config.conf[name][demod]["snapInterval"]`.

### 5.2 What this unlocks cheaply

- **Band up / band down** as a keyboard shortcut and as rigctl `BANDSELECT` —
  the gap Hamlib documents on the Icom side (`ic7300.c`: `RIG_PARM_BANDSELECT`
  disabled pending Icom documenting `0x1a 0x01`). Both become one call over the
  ordered bucket table.
- **Thetis-style register overlays on the waterfall** for entries inside the
  visible span — `registersInSpan()` is already in the sketch above. The
  research note calls this the one genuinely novel UI idea in the survey.
- **A second VFO's display state** later, keyed `(bucket, vfoName)`, without
  touching the register model.
- **`doc/todo/eibi-schedules-module.md`**: a schedule browser wants exactly
  `bucketOf()` and `registersInSpan()`.

---

## Part 6 — Alternatives considered

### 6.1 Keying by plan band identity instead of buckets

`"<plan>|<name>|<startHz>"` would fix duplicate names and plan-name collisions
with no new table. Rejected because it still leaves: overlap ambiguity on
write-back (needs a separate tie-break rule anyway), stacks orphaned on plan
switch, a single giant `GEN`, and per-plan-segment display calibration — 326
noise-floor buckets for netherlands.json, and 15 for the default plan's
shortwave. The bucket table is ~40 rows of static data and removes all four.

### 6.2 Splitting "where I was" from the curated registers ("Quisk layer")

Add `BandState.last{freq, mode}` — one tuple per bucket, exactly Quisk's
`bandState = {band: (centre, offset, mode)}` — written on every commit, while
`regs` are only written when the user is *bound* to a slot by an explicit recall.
A band key then recalls `last` when it is newer than the last recall, and the
registers stay purely curated.

This removes the dwell-commit trade-off in §3.4 entirely and is strictly more
faithful to "returning to a band restores where I was". It is not the P1
recommendation only because it adds a second recall-precedence rule that users
have to model ("why did tapping 40 m not go to my FT8 register?"), and because
neither Icom nor Thetis does it. Reconsider if pass-through overwrites annoy in
practice — the data model change is additive and the migration is trivial.

### 6.3 Per-entry bandwidth / filter, and a second stack per VFO

Both explicitly rejected in the research note (§5) and not revisited here.
Bandwidth belongs to the per-demod profile — this is Icom's own layering, where
the register stores a filter *slot* (FIL1/2/3) rather than a width. Thetis is
multi-receiver and still keeps one stack, tracking RX1 only.

### 6.4 A JSON bucket table in `root/res/`

Rejected: the migration and every stored key depend on bucket ranges being
stable, and a user edit would orphan stacks with no diagnostic. Revisit as an
additive `extraBuckets` if a regional need appears.

---

## Part 7 — Staging and verification

### P1 — correctness (the only part that fixes bugs)

Built: the data-layer seam (Part 8), stable band IDs, rotating-top
registers, scoped visible resolution, current-service-locked shutdown/lifecycle
commits, the `SET_MODE` guard and the frozen register popup. Remaining: the
bucket table with `GEN`, and shadow tracking. Closes research gaps 1–3 and
findings §1.4/1 and 4.

Verify in a real build:

- [ ] A missing or malformed `stackingRegisters` entry is treated as three
      empty slots; every newly written stable band contains exactly three array
      positions with `null` for empty slots.
- [ ] Tune around 20 m for a while, tap 40 m, tap 20 m → back where you were,
      and the other two 20 m registers are **unchanged**.
- [ ] Tune to 446.05 MHz, tap another band, tap PMR446 → returns to 446.05, and
      the 70 cm register is untouched.
- [ ] Tune to 9.500 MHz (31 m SW), tap 40 m, tap the 31 m segment → returns to
      9.500.
- [ ] Tune to 15.000 MHz (WWV, in no plan band and in no bucket), tap 20 m, then
      tap GEN → returns to 15.000. *Not 5.000 MHz, the obvious choice: 5.000 is
      in no plan band but it does fall in `SW60m` (4.700–5.100), because that
      bucket's outward rounding closes the 4.995–5.005 guard gap that HFBC
      leaves for WWV. Of the six WWV/WWVH channels only 2.5, 15 and 20 MHz reach
      `GEN`; 5 lands in `SW60m`, 10 in `30m` and 25 in `12m`.*
- [ ] Tap a band whose stored mode equals the current mode → no audible click
      (the `SET_MODE` guard).
- [ ] Tune by wheel only, quit, restart, tap that band → the wheel-tuned
      frequency is restored.
- [ ] Android: tune, background the app, force-stop it, reopen → frequency
      restored.
- [ ] Switch band plan from general to usa → stacks still recall.

### P2 — the register UI

Repeat-tap cycling and cyclic long-press row selection are implemented. The
three fixed optional entries are populated through normal tuning, transitions,
and lifecycle save; there is no add/store-here action. Lock/label/delete
metadata remains optional future work. Keeping the selector open after a
selection is the final optional polish step, after the current behavior is
verified.

### P3 — per-band display

`BandState.display`, slider capture, sticky-auto-range calibration capture,
display-menu toggle, `GEN` calibration. Closes gap 6.

### P4 — reach

Band up/down shortcut, rigctl band select, waterfall register overlays, snap via
`RADIO_IFACE_CMD_SET_SNAP_INTERVAL`.

### Open questions

1. **Bucket granularity below 30 MHz.** The table in §2.2 follows Thetis's
   `Extended` tiling (separate `SW49m`, `SW31m`, … buckets) rather than Icom's,
   which dumps all shortwave into `GENE`. Recommended, because this fork has a
   KiwiSDR source and a planned EIBI module, so shortwave listening is a
   first-class use case. Confirm, or take the smaller ~25-row Icom-shaped table.
2. **Should the `Ham` grid page be driven from the bucket table** rather than the
   plan's amateur bands? It would give one key per ham band exactly like the
   IC-705 screen, with stable labels across plans and no duplicate-name
   weirdness in the grid — at the cost of ignoring regional allocations there.
3. **§6.2** — adopt the `last`/`regs` split now, or keep the simpler
   Icom/Thetis model and revisit?
4. **Aeronautical-HF and maritime-HF buckets.** §2.1 uses these two services to
   justify multi-range buckets and then §2.2 omits both, so 27 segments in
   france.json alone fall into `GEN`. Adding `AIRHF` and `MARHF` as multi-range
   buckets is the consistent move and costs two rows; the argument against is
   that their segments are narrow and numerous, so the ranges would need
   maintaining. Decide before the table is frozen, because bucket ids are the
   persistence key and adding one later re-homes existing registers.
5. **A `WWV` bucket.** Part 4 cites Thetis's `_gen` / `_wwv` / `_xvtr` split as
   the precedent to copy, but the table has no `WWV`, so the standard-frequency
   channels scatter across four buckets (see the Part 7 note). One bucket owning
   2.5 / 5 / 10 / 15 / 20 / 25 MHz ±5 kHz, ordered before the HF buckets, would
   collect them — and per-band display calibration for a known-strong carrier is
   exactly the kind of thing Thetis wanted `_wwv` for.

---

## Part 8 — The data-layer seam

The band-stacking data layer is `gui::bandStack` (`band_stack.{h,cpp}`),
separate from the frequency-input widget. Everything in Parts 2–4 lands on this
seam without touching the widget layer except to render new affordances.

### 8.1 Division of responsibility

The widget renders band keys and register lists and reports user gestures;
every *decision* — which register, which frequency, which mode, what to write
back, what a first visit means — is made in `BandStack`, which the rest of the
app can also reach.

Not in it yet: the bucket table (§2.2), the poll-based shadow (§3.1), the dwell
timer (§3.4), GEN, locks, per-band display (Part 4), and the snap interface
command (§3.6). None of them require re-cutting this seam.

### 8.2 Why the seam is not a passive store

A passive store — `getRegisters(band)` / `putRegister(band, freq, mode)` —
would leave band selection's orchestration in the widget. That orchestration
**is** the state machine (write-back, then target resolution, then mode and
snap application), and everything later — the shadow, the dwell timer, the
display state — would have nowhere to live except the widget.

So the cut is by *direction of information*:

> The widget expresses **intent**; the data layer **decides and acts**.

Widget → data layer: `selectBand()`, `selectLegacySegment()`,
`openRegisters()`, `recallRegister()`. Data layer → world: request a tune, set
the mode, set the snap.

### 8.3 How the data layer tunes

`BandStack` follows `frequency_manager::applyBookmark()`: it applies the
recalled mode and channel spacing first, then calls
`tuner::tune(TUNER_MODE_NORMAL, vfoName, frequency)` directly. Band selection
runs on the UI thread, so no deferred mailbox or private tuning wrapper is
needed. The ordering matters because a mode change may rebuild the demodulator
and alter the VFO reference or bandwidth.

It does not tune by writing the widget's own state (`setFrequency()` plus
`frequencyChanged = true`): the band picker is drawn after `MainWindow::draw()`
consumes that flag, so the request would wait a frame, and a recalled mode
change can reconfigure the VFO in between, whose handling clears
`frequencyChanged` and drops the recall.

### 8.4 Where the pieces live

| Piece | Lives in |
|---|---|
| Registers, band selection, recall, popup snapshot, lifecycle commit, mode heuristic, mode and snap application | `band_stack.{h,cpp}` |
| Band grid page, category row, label helpers (`mhzExact()`, `wavelengthToken()`, `mhzLabel()`, `centeredLabel()`) | `freq_input/bands.cpp` |
| Keypad page | `freq_input/keypad.cpp` |
| `segButton()`, shared with the page toggle | `gui/widgets/simple_widgets.h` |
| Haptic feedback | the widget's two gesture handlers, so `band_stack.cpp` needs neither `android_backend.h` nor its `#ifdef` |

The widget makes no module-com calls and does not touch
`gui::waterfall.vfos`. It still includes `radio_interface.h` transitively,
because `band_stack.h` does: `BandRegister::mode` *is* a `RADIO_IFACE_MODE_*`,
and the widget names a mode for display. Mode names come from the canonical
table in `radio_interface.h` (`radioModeName()` / `radioModeFromName()`); see
`doc/design/radio-modes.md`.

### 8.5 The header

`band_stack.h` is the reference. Its shape: `BandRegister` (`freq`, `mode`),
`BandRegisterSet` (three optional slots with the rotation rules), the frozen
`BandRegisterPopupSnapshot` and the pure `prepareBandRegisterPopup()`
transition that `band_register_set_tests` covers, and the `BandStack` class
with an explicit UI-thread-only threading contract: nothing in it is called
from the tune path, so rigctl's network thread (`doc/bugs/ui-thread-sync.md`)
never reaches this state — the current frequency is sampled, never pushed in.

**Why a class rather than free functions,** given the config is the store: the
state P1 adds next — the shadow and the dwell timer — is per-application state
that must be reachable from `MainWindow::draw()`, `core.cpp`'s shutdown and the
Android pause handler. `WaterfallAutoRange` is the in-tree precedent: also
small, also a class because it owns a latch and a timer and is driven from the
frame loop.

### 8.6 Instance and build wiring

- `core/src/gui/widgets/band_stack.{h,cpp}` — beside `bandplan.{h,cpp}` and
  `band_mapping.{h,cpp}` because all three jointly interpret band-plan rows,
  stable identities and the selector's band-memory behavior.
- `gui.h`: `SDRPP_EXPORT BandStack bandStack;` alongside `freqSelect`. Exported
  because out-of-tree consumers will want it (rigctl band select, §5.2).
- `core/CMakeLists.txt` needs no entry — `file(GLOB_RECURSE SRC "src/*.cpp")`
  picks it up, including `widgets/freq_input/`. The glob has no
  `CONFIGURE_DEPENDS`, so adding a source file requires a CMake re-configure
  before it is built.

### 8.7 Plan resolution

Both the band grid and the data layer read `gui::waterfall.bandplan`, the app's
resolved plan, set by `bandplanmenu::init()` (`menus/bandplan.cpp`) and updated
by the plan combo. So the grid and the waterfall ruler always show the same
plan, including when `conf["bandPlan"]` names a plan that is not loaded (both
fall back to `bandplanNames[0]`), and no call threads the plan through. The
"independent of the `bandPlanEnabled` toggle" requirement in
`doc/todo/band-picker.md` holds — that flag controls visibility, not the
pointer.

### 8.8 Where P1 lands

Further band-stack work touches the widget layer only to render new affordances
(lock icons, cycling feedback), in `freq_input/bands.cpp`:

| P1 item | Where it lands |
|---|---|
| Bucket table, `bucketOf()` | new file-static table in `band_stack.cpp`; replaces the visible-service resolution behind write-back |
| Shadow, dwell timer | new private members; new public `update(double dt)` |
| Poll-based tracking | `gui::bandStack.update(dt)` in `MainWindow::draw()`, beside `autoRange.update()` |
| Shutdown / Android pause commit | already `freq_memory::commitCurrent()` → `commitCurrentBand()`; extended to commit the shadow |
| Migration | free `bandstack::migrateConfig(json&)` called from `core.cpp` before the unused-key repair (§2.6) |
| Per-band display | `BandState` members + `captureDisplay()` called from the Ref/Range slider handlers |
