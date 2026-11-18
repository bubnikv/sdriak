# cw_engine: clean-up / refactoring backlog

Proposals from a review of `decoder_modules/cw_engine` (2026-11-16). Nothing
here is done. Items 2, 3 and 5 should keep the gate output byte-identical;
item 4's eager float pen table and `std::` float math may change bits and
need their own baseline review.

Suggested order: 1 → 2 → 3 → 4 → 5 → 6.

## 1. The regression gate in the tree (do first)

Every refactor so far was checked against a byte-identical cw_test /
call_test baseline that lives only in a temporary scratchpad (`tbuild/check.sh`,
`baseline/`). Move it into the tree:

- `decoder_modules/cw_engine/test/golden/` with the cw_test and call_test
  outputs;
- a CTest that runs `cw_engine_cw_test` / `cw_engine_call_test` and diffs
  against them.

Everything below lands only with that gate green.

## 2. Const correctness

- **Pen memo makes readers non-const.** `CwLattice::pen()` writes the memo,
  so `CwReadView::lattice` is `CwLattice *` and `CwState::read_view()` is
  non-const. The memo is a cache: make `pen`, `pen_at`, `pen_gen`,
  `pen_dit/csp/wsp/esp/edge` `mutable` (or the ring's memo a separate mutable
  member) and `pen()`/`sync()` const. Then `CwReadView::lattice` becomes
  `const CwLattice *`, `read_view()` const, and the re-read / call reader
  cannot write the lattice.
- **`CwWordReread::forced()` / `free_reading()`** only read `m_oracle_loose`
  → `const`. Check the `CwCallReader` helpers for the same.
- **`SkimCallsignExtractor::best()` has a side effect**: it refreshes
  `m_x->dict` from the source, while the const `top()`, `hearings()`,
  `addressed()` use whatever snapshot the last `best()` took (`top()` can see a
  stale dictionary). Take the snapshot once per unit of work in `feed()` (or an
  explicit `refresh()`), make `best()` const.
- **String APIs**: `const char *` → `std::string_view` on `SkimCallsign::is_valid`,
  `SkimCallsignDict::has/contains/near`, extractor `feed/hearings/addressed/
  last_heard`. `contains()` builds a `std::string` per lookup (heterogeneous
  unordered lookup is C++20-only); at least stop repeated `strlen` in the
  internal callers.

## 3. Remaining out-parameters → return values

- `CwSegModel::window(type, int*, int*)` → `struct Window { int lo, hi; }`
  (`auto [lo, hi] = sm.window(t)`; 5× per sample in `CwLattice::step`, and
  every re-read loop).
- `CwLattice::head(..., uint64_t*, bool*)` → `{tau, mark_ended}`.
- `CwPreroll::find_head(..., unsigned*, unsigned*)` →
  `std::optional<Replay{have, start}>`.
- `CwClock::est_cluster(..., double*)` → `std::optional<double>`.
- callsign.cpp: `fused_markers(..., bool *cq, bool *de)` and the `reps_out`
  helper.
- `(array, count)` pairs — `kr_add(SkimCall*, unsigned*, ...)` /
  `kr_add_near` in cw_callreader.cpp, `cand`/`nc`, `SkimCallsignDict::near()`
  output, `m_kr_hit` — → a small no-heap `FixedVector<T, N>` (`push_back`
  returns false when full), which also absorbs the
  `MAX_NEAR_CALLS` / `MAX_CALL_SEGMENTS` bookkeeping.

## 4. Performance

- **`CwLattice::step`**:
  - compute the 5 windows once per `sync()` generation instead of per sample.
    `window()` also reads `elem_err` and `dah_max`, which are not in the sync
    key — add them (or `elem_err` bucketed by its 0.20 threshold, its only
    use);
  - clamp `dhi` to `t - cmt` before the loop instead of testing
    `t - d < cmt` per iteration.
- **Pen memo**: `double pen[5][1025]` + `uint32_t` stamps = 61.5 KB per active
  channel, mostly outside the windows. With the windows cached per generation,
  fill `pen[type][lo..hi]` eagerly on a generation change: no stamps, no
  branch in the inner loop, and `float` (scores are float anyway) — roughly
  halves the memo (the ARM footprint lever). May change bits.
- **Per-call heap scratch** on the real-time thread: `CwWordReread::reread`
  allocates `std::vector`s on every word close (cw_reread.cpp ~131, ~210);
  `CwCallReader::read` 4–5 vectors every 3 s (cw_callreader.cpp ~133, ~275,
  ~308). Make them reused member buffers (`resize`, never shrink).
- **Unqualified math**: ~32 `fabs`/`log`/`exp`/`sqrt` calls without `std::`.
  Qualify them; where the operand is float the float overload then applies,
  which may change bits — check against the gate, keep `double` where it
  diverges.

## 5. Structure / compile hygiene

- **Header-only stages**: `cw_frontend.h` (713 lines), `cw_clock.h`,
  `cw_preroll.h`, most of `cw_lattice.h`. Move non-trivial bodies
  (`CwCarrierDetector::hop`, `fft`, `CwPowerStats`, `CwClock::relock`,
  `CwPreroll::find_head`) into `cw_frontend.cpp` / `cw_clock.cpp` /
  `cw_preroll.cpp` so the headers read as interfaces. Keep
  `CwLattice::step`/`pen` inline (hot).
- **Split `callsign.cpp` (1481 lines)**:
  - `callsign_valid.cpp` — ITU tables, `core_valid`, designators;
  - `callsign_dict.cpp` — load/parse/inspect, near index;
  - `callsign_extract.cpp` — tokenizer, `Cand`, scoring, fused/despace
    fallbacks.
- **`skim_common.h` is a catch-all** (`<mutex>`, `<numeric>`, `<iterator>`…
  everywhere): include-what-you-use per file; keep it for `SKIM_PI`.
  `skim_strlcpy`/`skim_strlcat` have few callers left after `SkimCall` — move
  those to `std::string` / `SkimCall` and delete the helpers.
- **`SkimDecode::text[64]` + `CwText`**: if the module side needs no POD,
  `std::string text` (reserved to `kTextMax`) removes `CwText::fits/put`. The
  "whole or not at all" `put(const char *)` matters for the FIX brackets, so
  keep a `try_append(string_view)`.
- **FIX phrase buffer** (`CwCallReader::m_fix_phrase`, char array) →
  `std::string`.
- **Debug formatting**: the `char t_[48]; snprintf(...); g += t_;` pattern
  (cw_callreader.cpp, cw_reread.cpp, cw_rundump.h) → one
  `append_fmt(std::string &, fmt, ...)` helper.

## 6. Smaller C++-isms

- `static const CwSeg MTYPES[2]` / `STYPES[3]` in `CwLattice::step`, `MT`/`ST`
  in `CwWordReread::free_reading` → `static constexpr std::array`, range-for.
- `SkimCallsignDict::m_release[32]`, `SkimCallsignDictInfo::release[32]` →
  `std::string` (not hot).
- `shared_ptr<SkimCallsignDict>(new ...)` (callsign.cpp) and
  `unique_ptr<SkimChannelizer>(new ...)` (channelizer.cpp) stay — private
  constructors; add a one-line comment saying so.
- `std::atomic_load/atomic_store` on `shared_ptr`
  (`SkimCallsignDictSource`) is deprecated in C++20 → `std::atomic<std::shared_ptr>`
  when the engine moves to C++20.
