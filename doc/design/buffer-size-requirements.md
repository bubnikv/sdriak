# Buffer size requirements — `STREAM_BUFFER_SIZE` and producer block sizes

Date: 2026-09-12. Resampler audit updated 2026-09-13. §2 recomputed
2026-09-13 for the kernel/block split, then again for rate-aware sizing (§2.4).

Status: **preliminary diagnosis — not a completed requirements analysis.** The
WFM arithmetic in §2 is recomputed against the current tree. The capacity audit
in §3 is not complete enough to justify changing `STREAM_BUFFER_SIZE`, and §7
lists what must land first. Do not use §5.3's table as a safety case.

Three things have landed since the first revision. None of them lowers the
constant, and §7 is still the gate:

* the rational-ratio explosion in §1.4 is fixed in `rational_resampler.h`;
* the **kernel/block split** (§2.1) removed 18 of the 23 `dsp::stream`s the WFM
  demodulator used to construct, and with them the initialisation spike that
  §2.2 was written to explain. The numbers throughout §2 and §5.3 are the
  recomputed ones; the previous revision's figures are superseded, not adjusted;
* the raw-pointer members §5.2 listed as blocking recoverability now have RAII
  owners, which changes what is left to do there but not the conclusion in §5.1;
* **rate-aware sizing** (§2.4) took 88 MB off the WFM demodulator and a further
  ~42 MB off the rest of a running radio session, by sizing each buffer for the
  rate its own branch runs at. This one *is* relevant to the constant: it is the
  first change that makes a buffer's size depend on something other than
  `STREAM_BUFFER_SIZE`.

Written from an `armeabi-v7a` startup crash (SIGSEGV in `memset`, reached
through `demod::WFM::init`) that turned out to be an out-of-memory condition,
not a pointer bug.

`STREAM_BUFFER_SIZE` (`core/src/dsp/stream.h:10`) is currently `1 000 000`
elements. It looks like a tuning knob. It is not: it is the implicit capacity
contract between every producer in the DSP graph and every stream they write
into, and it is a large part of the application's native memory footprint.

See `doc/design/dsp-pipeline.md` for how streams and blocks fit together; this
document only covers sizing.

---

## 1. The constant is a default, not an enforced invariant

A `dsp::stream<T>` allocates two buffers of `STREAM_BUFFER_SIZE` elements in its
**constructor** (`core/src/dsp/stream.h`). A producer writes into
`stream.writeBuf` and only afterwards calls `swap(n)` to publish `n` samples.

Nothing records the allocated capacity, and nothing checks `n` against it. Even
if it did, `swap(n)` runs *after* the write, so a check there detects corruption
rather than preventing it.

> If any producer writes more than `STREAM_BUFFER_SIZE` elements into a
> `writeBuf`, the result is a silent heap overflow.

Two consequences shape the rest of this document.

**Source maxima cannot prove the invariant.** §3 audits what each source module
produces, and that is necessary but not sufficient, because sources are not the
only producers.

**Internal blocks can expand, and shipped chains do.** The resampler audit below
found one shared capacity defect in every streaming interpolator, several live
expanding paths, and two independent defects in the currently unused RRC
interpolator.

### 1.1 The shared resampler defect

All rational and RRC resampling eventually reaches
`PolyphaseResampler::process()`
(`core/src/dsp/multirate/polyphase_resampler.h:69`). It performs two unchecked
writes:

* `memcpy(bufStart, in, count * sizeof(T))` copies the input into a working
  allocation of `STREAM_BUFFER_SIZE + 64000` elements (`:31,73`); and
* the convolution loop appends to `out[outCount++]` without receiving or
  checking an output capacity (`:78,81`).

For interpolation ratio `L/M`, the number of output elements is approximately
`ceil(count * L / M)`, with the exact count also depending on the retained
polyphase state. `run()` learns `outCount` only after `process()` has written the
result (`:105`), so checking `swap(outCount)` cannot prevent an overflow.

`RationalResampler` wraps this implementation. Its downsampling modes do not
expand a valid input block and are therefore bounded if both streams have the
same capacity. `RESAMP_ONLY`, used for upsampling, has no such property.
`RRCInterpolator` delegates directly to the same `process()` implementation.

The extra 64 000 elements in the working allocation are filter history, not
general-purpose output headroom. The implementation does not assert either
`count + tapsPerPhase - 1 <= maxBlock + 64000` or that the caller's output
allocation can hold the predicted result.

`maxBlock` is new, and is the only part of §7.1 that exists today. The fused
kernels — `FIR`, `Delay`, `PolyphaseResampler`, `PowerDecimator`,
`RealToComplex` — now take it as an `init()` argument instead of reading
`STREAM_BUFFER_SIZE` directly, so a composite can size its working storage for
the block it will actually be handed. **Every call site still leaves it
defaulted**, so nothing has changed operationally; what has changed is that the
plumbing for a per-destination bound no longer has to be invented, only used.
The argument bounds the *input* block a kernel accepts. It says nothing about
the caller's output allocation, which is the unchecked half of the defect above.

### 1.2 Complete use-site inventory

The current tree contains these actual resampler users:

    owner/path                    conversion                         assessment
    ---------------------------   --------------------------------   -------------------------------
    signal-path RxVFO             source -> requested VFO rate       normally down; can interpolate
    RadioModule AF chain          demod AF -> audio-device rate      live interpolation
    M17 audio                     8 kHz -> audio-device rate          live, bounded small input frame
    radiosonde decoder            10/15/20/50 kHz -> 48 kHz          up to 4.8x; no enforced bound
    BroadcastFM RDS               demod rate -> 5 kHz                 downsampling
    CTCSS detector RxVFO          demod AF rate -> 500 Hz             downsampling
    VOR internal RxVFOs           25 kHz -> 1 kHz                     downsampling
    FobosSDR source DDC           device rate -> selected rate        downsampling
    SDDC source DDC               2x selected rate -> selected rate   downsampling
    RyFi transmitter              baud rate -> baseband rate          interpolates; no live owner
    WebSDR small waterfall        currently 12 kHz -> 12 kHz          direct vector output; bounded
    modulator GFSK/PSK            symbols -> baseband                 currently no call site

All calls to `vfoManager.createVFO()` use `RxVFO` and therefore inherit its
resampler. The call sites are Radio, M17, Meteor, Radiosonde, Weather Satellite,
VOR, RyFi, KG-SSTV, Falcon 9, DAB, Pager, ATV and IQ Exporter. The intended case
is decimation, but neither `IQFrontEnd::addVFO()` nor `RxVFO` rejects a requested
output rate above the current source rate. In that case the VFO becomes another
unchecked interpolator. This is particularly important for arbitrary or low
source rates.

The `rational_resampler.h` includes in DAB and Pager do not instantiate a
resampler and are stale for purposes of this audit.

On Android, the hard-coded loader includes the live resampler users M17,
Meteor, Radio, Radiosonde and WebSDR View. ATV, Falcon 9, DAB and the other
decoder call sites above are not loaded there.

### 1.3 Radio AF interpolation: code bound versus operational bound

The radio audio-chain resampler runs from the demodulator's AF rate to whatever
rate the sink reports (`radio_module.h:569,616`). At a 192 kHz sink:

    demod       AF rate   expansion   max input at capacity 262 144
    ----------  --------  ----------  --------------------------------
    CW/CWR         3 000       64x                      4 096
    AM            15 000     12.8x                     20 480
    USB/LSB/DSB   24 000        8x                     32 768
    NFM           50 000     3.84x                     68 266 (conservative)
    WFM          250 000    0.768x                     downsampling
    RAW          sink rate        1x                    no expansion

Therefore a standalone CW AF block larger than `capacity/64` would overflow.
That is a real missing invariant, but it is not the same as saying an audited
source callback is multiplied by 64. The callback first passes through the VFO
which converts the source rate to 3 kHz. For a continuous chain the two ratios
approximately cancel:

    source block * (3 kHz / source rate) * (192 kHz / 3 kHz)
      = source block * (192 kHz / source rate)

For example, a 131 072-sample HackRF callback at 2 MHz becomes about 12 583
audio samples at 192 kHz, and a 512-sample KiwiSDR block at 12 kHz becomes
8 192. Both fit a 65 536-element stream. This explains why the 64x path works
today and materially weakens it as evidence for choosing 262 144 over 65 536.
It does not establish safety: no code enforces a maximum block duration, and a
large low-rate network block can still expand past the destination.

M17 is also operationally bounded: its Codec2 decoder publishes
`2 * codec2_samples_per_frame()` samples at 8 kHz, approximately 320 in the
configured mode, so even 24x conversion to 192 kHz produces only about 7 680
samples. Radiosonde is less well specified: its 10 kHz mode interpolates 4.8x
to 48 kHz and relies on the upstream VFO block being small; there is no explicit
limit.

Chunking a source only to the immediate stream capacity is not sufficient when
a downstream VFO or AF stage can interpolate it. A correct chunk limit must
also account for downstream expansion, or the expanding block itself must
fragment its output.

### 1.4 Rational-rate allocation explosion — FIXED

`RationalResampler::reconfigure()` used to round both rates to integers and
reduce them with `std::gcd`. Tap generation scales with the resulting
interpolation factor, and `buildPolyphaseBank()` allocates **one separate
buffer per phase**, so a poor common divisor produced an enormous FIR followed
by that many individual allocations.

The failure was far more common than "arbitrary nearly equal rates". The
predecimated rate `_inSamplerate / predecRatio` is usually **fractional** —
2 500 000 / 64 = 39 062.5 — and rounding it is what destroyed the common
factor: `gcd(39063, 24000)` is 3 where the true ratio reduces to 384/625. So
ordinary hardware hit it:

    configuration              interp    taps    taps+bank   allocations
    ------------------------   ------  -------   ---------   -----------
    RTL-SDR 2.4M -> USB 24k        16    1 900      7.7 KB            16
    Airspy 2.5M -> USB 24k      8 000  989 583     4 032 KB         8 000
    Airspy/SDRplay 10M -> USB   8 000  989 583     4 032 KB         8 000
    Airspy 2.5M -> CW 3k        3 000  371 093     1 512 KB         3 000
    BroadcastFM rdsResamp       5 000  593 750     2 420 KB         5 000

2.4 MS/s divides cleanly by 64, so RTL-SDR was fine; 2.5 and 10 MS/s do not.
`BroadcastFM`'s own RDS resampler hit it unconditionally, on every platform, at
every source rate — 250 kHz -> 5 kHz is hardcoded. The induced rate error was
0.0013–0.0064 %, just under the 0.01 % warning threshold, so it was silent.

**The fix** derives the ratio exactly, as `_outSamplerate * predecRatio` over
`_inSamplerate` in 64-bit, and caps the phase count at `MAX_INTERP = 512` using
the convergents of the continued fraction expansion plus the best
semiconvergent (`approxRatio()`). The cap is on `interp` **only**: `decim` is
just the stride of the phase accumulator and costs no memory, and bounding it
would spoil exact ratios such as 384/625.

    instance                       context               before      after
    ----------------------------   -------------------   ---------   --------
    BroadcastFM rdsResamp          WFM, fixed rates      4 775 KB    15.3 KB
    RadioModule afChain resamp     WFM AF -> 48k            76 KB      76 KB
    CTCSSSquelch ddc.resamp        NFM AF -> CTCSS 500     478 KB    15.3 KB
    RxVFO, RTL 2.4M                -> WFM IF               3.7 KB     3.7 KB
    RxVFO, Airspy 2.5M             -> USB IF             7 958 KB     382 KB
    RxVFO, Airspy 2.5M             -> CW IF              2 984 KB     382 KB
                                                         --------   --------
                                                         16.28 MB   0.875 MB
                                                        16 607 alloc  907 alloc

Every already-correct configuration is bit-identical, and the rate error on all
hardware paths is now **exactly zero** — the fix is strictly more accurate, not
a trade. Only genuinely coprime pairs are approximated, and better than the
error the code already tolerated: a 44 101 Hz file to 48 kHz gives 357/328,
0.00036 % error, 111 KB instead of 14 976 KB.

Non-positive rates are also now rejected before the GCD: they yielded
`interp == 0`, which reached `buildPolyphaseBank()` as a zero phase count — a
division by zero computing `tapsPerPhase` and a zero-sized allocation.
`reconfigure()` stays in `Mode::NONE` instead.

Network Source still accepts arbitrary integer sample rates (§4), but that is
now a block-size problem rather than an allocation explosion.

### 1.5 Dormant RRC defects — FIXED — and test coverage

Both defects the previous revision recorded here are fixed, and both were
byproducts of the kernel split rather than deliberate repairs:

* `RRCInterpolator::init()` assigned `rrcBeta` to `_rrcBeta` only *after*
  calling `genTaps()`, which therefore read an indeterminate value. The
  assignment now precedes the call (`rrc_interpolator.h:20-23`).
* The class had no destructor freeing `rrcTaps`, so its final tap allocation
  leaked. The member is gone: the taps are a temporary that
  `PolyphaseResampler` copies into its own phase bank (§6.1).

`RationalResampler::Mode::NONE` now skips the copy when `out == in` rather than
relying on identical-pointer `memcpy` behaviour — which it had to, because the
fused `RxVFOKernel` passes the same pointer for both.

`rational_resampler_tests` now exercises output counts against the configured
bound across retained phase state, including decimation, interpolation,
fractional conversion, and the near-unity approximation that can select
passthrough. Large interpolation ratios, rate reconfiguration, and the RRC
initializer remain uncovered. Those tests are still required before a smaller
global capacity can be treated as safe.

The modules in §3.3 clamp against the constant themselves and so track it
automatically. Everything else in §3.1, §3.2 and §4 does not.

---

## 2. What the constant costs — for one demodulator

These are **live requested allocation bytes attributable to the WFM
demodulator**. They are not process totals, and they are not resident memory —
see §2.3.

Per `dsp::stream<T>`, at 1 000 000 elements:

    element type    sizeof   bytes per stream (2 buffers)
    -------------   ------   ----------------------------
    complex_t            8   16 MB
    stereo_t             8   16 MB
    float                4    8 MB
    uint8_t              1    2 MB

### 2.1 Construction, and the kernel/block split

The counting rule used to be "every DSP block owns one output stream, so every
block costs 8–16 MB merely by being constructed", with a footnote to count
nested processors: `RationalResampler<T>` held a `PowerDecimator<T>` and a
`PolyphaseResampler<T>` and therefore cost **three** streams, not one.

That rule no longer describes the tree. Most of what used to derive from
`dsp::Processor` is now a plain **computation kernel** — no output stream, no
worker thread, no mutexes — because it was never a chain node to begin with: the
containing composite called its `process()` inline and freed the stream it had
just paid 8–16 MB for, or simply left it allocated and unused.

    kernels now (no stream)     FIR, DecimatingFIR, Delay, FrequencyXlator,
                                RxVFOKernel, Quadrature, RealToComplex, AGC,
                                FastAGC, PLL, CarrierTrackingPLL, Costas, MM,
                                DifferentialDecoder, PolyphaseResampler,
                                PowerDecimator, RationalResampler,
                                RRCInterpolator, MeteorCostas

    still blocks (one stream)   the demod composites (AM, FM, BroadcastFM, SSB,
                                CW, GFSK, PSK), DCBlocker, Deemphasis,
                                NoiseBlanker, PowerSquelch, FMIF, CTCSSSquelch,
                                Splitter, Reshaper, SampleFrameBuffer, and the
                                threaded adapters FIRBlock, FastAGCBlock,
                                PowerDecimatorBlock, RationalResamplerBlock,
                                RxVFO

Twelve of the fourteen `filter::FIR` instances in the tree are fused; the two
that really are chain nodes (`RadioModule::hpf`, the RyFi transmitter's `rrc`)
use `FIRBlock`. The naming is inverted for the VFO only because `RxVFO` is
public API (`vfo_manager.h`, `iq_frontend.h`) and had to keep its name, so the
kernel is `RxVFOKernel`.

**The counting rule is now: count blocks, not sub-objects.** A kernel member
costs nothing at construction; only its `init()` allocates, and only a working
buffer.

    dsp::demod::BroadcastFM            2 streams         32 MB
      8-byte  (2): out(stereo), rdsOut
      (demod, pilotFir, rtoc, xlator, pilotPLL, lprDelay,
       lmrDelay, alFir, arFir and rdsResamp are all kernels)
    RDSDemod                           2 streams         10 MB
      4-byte  (1): soft
      1-byte  (1): out
      (agc, costas, fir, costas2, recov, diff are all kernels)
    Reshaper<float>                    1 stream           8 MB
                                                      -------
    constructed by `new demod::WFM()`                    50 MB

**284 MB to 50 MB, and 23 streams to 5.** The `sink::Handler` members (`hs`,
`diagHandler`) derive from `Sink`, which has no output stream, and cost nothing
in either revision.

This counts **streams only**. Three other allocations are not in the figure
above, and the split moved the balance between them:

* FIR, delay and resampler working buffers, sized `maxBlock + 64000`. Unchanged
  by the split — a kernel keeps the working buffer it always had. They are in
  the §2.2 trace.
* **Scratch buffers, which are new.** A fused kernel that used to write into a
  sibling's `out.writeBuf` now needs somewhere to put its output, so the
  composites allocate `dsp::buffer::Buffer<T>` scratch in `init()`. These are
  half the cost of the stream they replace — one buffer of `STREAM_BUFFER_SIZE`
  rather than a stream's two — and they are allocated at `init()` rather than at
  construction. BroadcastFM allocates 48 bytes per element across eight of them,
  RDSDemod 17 across three.
* Polyphase filter taps and the phase bank. These scale with the *resampling
  ratio*, not with `STREAM_BUFFER_SIZE`, so they are a fixed adder that a
  smaller constant does not reduce. Before the §1.4 fix `rdsResamp` alone
  contributed 4.78 MB here in 5 001 separate allocations, which no revision of
  this document counted; it is now 15.3 KB in 17.

### 2.2 Peak during initialisation

`WFM::init()` then allocates working buffers, scratch and taps. **The
initialisation spike is gone.** It existed because
`PowerDecimator::reconfigure()` builds its stages with `new DecimatingFIR<...>`,
and `DecimatingFIR` derives from `FIR`, which was then a `Processor`: each stage
constructed a 16 MB `out` stream *and* an 8.5 MB working buffer, then freed only
the stream — a 16 MB transient per stage that never appeared in the end state.
`FIR` is now a kernel, so a stage costs its working buffer and nothing else.

    after construction                                     50.0
    + pilotFir 8.5, nullBuf 4.0, lprDelay 4.3, lmrDelay 8.5,
      alFir 4.3, arFir 4.3                                 83.8
    + rdsResamp.init -> decim.init(ratio 2), plan_2,
      one stage, working buffer only                        92.3
    + resamp working buffer                                100.8
    + reconfigure() -> decim.setRatio(32): 3 x plan_32
      stages built before the plan_2 stage is released,
      so both are held across the swap        transient    126.4
      old stage released                                   117.8
    + reconfigure() -> rdsResamp taps + phase bank         117.8   (+0.015)
    + BroadcastFM scratch (demodOut, rtocOut, pilotFirOut,
      pilotPLLOut, lmrDelayOut, lmr, l, r)                 165.8
    RDSDemod::init (fir 8.5, recov 4.0, scratch 17.0)      195.4
    reshape.init (RingBuffer<float>, RING_BUF_SZ)          199.4   <-- peak

250 kHz -> 5 kHz gives `predecRatio` 32, so the plan is `plans[4]` = `plan_32`,
which has three stages (`core/src/dsp/multirate/decim/plans.h:76`) and therefore
retains 25.5 MB of FIR working buffers.

The one remaining transient is at `decim.setRatio(32)`: `reconfigure()` builds
the replacement stage vector into locals and commits only once every allocation
has succeeded, so the plan_2 stage and the three plan_32 stages are held
simultaneously. That is deliberate — it is what makes a failed reconfiguration
leave a working decimator rather than an empty one — and at 126.4 MB it is well
below the end state, so it no longer sets the peak.

**50 MB constructed, ~199 MB peak, ~199 MB still held afterwards — for one
demodulator**, in 4–16 MB chunks needing contiguous virtual address space.
Previous revision: 284 MB constructed, ~342 MB peak, ~232 MB retained.

The shape of the problem changed more than the size of it. Construction dropped
82 % and the peak 42 %, but **the retained figure only dropped 16 %**, because
the scratch buffers in §2.1 took over most of what the freed streams were doing
and the working buffers were never the streams' fault. Initialisation no longer
overshoots its own end state, which is the part that matters on a fragmented
32-bit address space — but the end state is still ~199 MB for one demodulator,
and that is what §5.3 has to move.

### 2.3 This is not the process peak

Two things the numbers above deliberately exclude.

**RadioModule allocates more than the demodulator, and allocates first.** Its own
members are constructed before `instantiateDemod()` is ever called
(`decoder_modules/radio/src/radio_module.h`, member block at the end of the
class). Applying the §2.1 rule — count blocks, not sub-objects:

    nb, fmnr, powerSquelch, dummyAudioStream       4 streams     64 MB
    ctcss  (CTCSSSquelch)                          1 stream      16 MB
      its own out (stereo); ddc is now an RxVFOKernel
      and fm a Quadrature kernel, so neither adds one
    resamp (RationalResamplerBlock<stereo_t>)      1 stream      16 MB
    hpf (FIRBlock<stereo_t,float>), deemp          2 streams     32 MB
    SinkManager::Stream: volumeInput, volumeAjust  2 streams     32 MB
                                                 -----------   -------
    10 x 8-byte                                   10 streams    160 MB

`CTCSSSquelch` used to be the trap here — it owned two composites and eight
streams, and its `fm` was the only 4-byte stream in the whole tally. It is now
one stream plus two scratch buffers (`ddcOut` complex, `fmOut` float). Previous
revision: 19 streams, 296 MB. `dsp::chain` and `routing::Splitter` hold only
pointers and cost nothing, in both revisions.

Its init-time allocations, with §2.4's rate-aware sizing applied (NFM: 50 kHz
AF, CTCSS decoding at 500 Hz, so a 100:1 drop across `ctcss.ddc`):

    fmnr (FMIF, 1024 bins)                 delay line       8.5 MB
    powerSquelch normBuffer                                 4.0 MB
    ctcss.ddc      resampler decimator stages              11.0 MB
                   resampler polyphase delay line           0.8 MB
                   post-resample filter delay line          0.6 MB  (was 8.5)
    ctcss ddcOut + fmOut                                    0.1 MB  (was 12.0)
    resamp (AF -> 48 kHz) decimator + polyphase            11.8 MB
    hpf delay line                                          8.5 MB
    deemp                                                   0.0 MB
                                                          --------
                                                            45.3 MB

    member construction (10 streams, §2.1 above)            160 MB
    retained after RadioModule DSP init                      205 MB
    retained + the WFM peak from §2.2 (205 + 111)            316 MB

Previous revision: ~296 / ~321 / ~344 transient / ~663 MB. The separate
"transient peak" line is gone for the same reason as in §2.2 — the streams that
were constructed only to be freed are no longer constructed.

**The 160 MB of streams is now the whole problem.** Every item in the init line
has been sized for the rate it runs at; what is left is ten full-capacity
`dsp::stream`s, eight of which decimate. The VFO's own output stream is the
clearest case — it converts the source rate to 250 kHz, so it carries about a
tenth of what it is built for — and it is blocked on §7.1 for the reason §2.4
gives: a stream's consumer is a different block, and the owner cannot stop it to
resize safely.

These figures are still derived by reading the code rather than measured, and
§7.7 still asks for a real tally.

The last line is the closest thing to a process figure in this document, and it
still excludes the VFO, the source module, waterfall history, other modules and
allocator overhead.

The whole-application construction peak is still not tallied: the VFO created by
`vfoManager.createVFO()` (`radio_module.h:92`) is not counted above, nor are the
front end, waterfall history, other modules or allocator metadata.

**Requested bytes are not resident bytes.** Everything here counts allocation
requests. Physical footprint (RSS/PSS), address-space fragmentation and
allocator overhead are separate quantities and have not been measured. On
32-bit, fragmentation of the 4 GB address space is often what actually fails a
large contiguous request — so "342 MB cannot fit" is not a universal claim, it
is a hypothesis that needs on-device measurement (§7).

### 2.4 Rate-aware sizing

The §2.2 trace exposed something the stream-counting never did. Grouping the
199.4 MB by **the sample rate the buffer actually runs at**:

    group                                                    MB     rate
    ------------------------------------------------------  -----   -----------------
    BroadcastFM out, 8 scratch, 5 FIR/Delay buffers, nullBuf  97.8   250 kHz (full)
    rdsResamp decimator stages + polyphase delay line         34.0   250 kHz -> 7.8 kHz
    RDS branch: rdsOut, all of RDSDemod, the Reshaper         67.5   5 kHz

**Half the total sat in branches running at 1/32 to 1/50 of the input rate,
sized as though they ran at full rate.** None of that needed the capacity
contract in §7 — the ratios are known at `init()`/`reconfigure()` time. Four
changes, all local:

    change                                                   saved
    ------------------------------------------------------   -------
    RDS branch sized for RDS_SAMPLERATE (5 kHz)               61.9 MB
    PowerDecimator stages sized by cumulative decimation      14.5 MB
    PolyphaseResampler sized for its post-decimation input     7.8 MB
    RealToComplex::nullBuf deleted                             4.0 MB
                                                             -------
                                                              88.2 MB

**199.4 MB -> 111.2 MB**, and construction is unchanged at 50 MB.

The RDS number is the interesting one. `BroadcastFM::rdsResamp` is 250 kHz ->
5 kHz, so `rdsOut` and everything behind it can only ever carry `maxBlock / 50`
samples — 20 064 with the slack for the resampler's retained phase. Sized for
that, `rdsOut` is 0.32 MB instead of 16, RDSDemod's two complex scratch buffers
0.32 instead of 16, its `soft` stream 0.16 instead of 8, its FIR delay line 0.67
instead of 8.5. `BroadcastFM::getRDSMaxBlock()` publishes the bound and
`RDSDemod::init()` takes it.

`RealToComplex` is a different kind of waste: it kept `maxBlock` zero-valued
floats purely to hand `volk_32f_x2_interleave_32fc()` an imaginary part, and
read all 4 MB of them on every block. Writing the zeros directly is less memory
*and* less memory traffic.

Two places where the obvious version of this is **not safe**, both for the same
reason — `stream::setBufferSize()` frees a buffer a running consumer may still
be reading, and `TempStop` stops a block but not its reader:

* `Reshaper::out` carries exactly `_keep` samples, so it looks like a free 8 MB
  in WFM (and ~15 MB in the IQ front end). But `setKeep()` cannot resize it:
  `iq_exporter` calls `setKeep()` straight from its menu handler while its sink
  runs. The bound is therefore opt-in at `init()` (`maxKeep`), taken by WFM
  where keep is a fixed 4096, and declined everywhere keep varies at runtime.
  The front end's 15 MB is left on the table until §7.1 lands.
* `BroadcastFM::setSamplerate()` grows `rdsOut` only, and only under a stated
  precondition that the caller has stopped the RDS consumer. It has no caller
  today — WFM's IF rate is the constant 250 kHz.

What is left is now genuinely full-rate, and shrinks only when a producer's
block size becomes knowable (§7 items 1-2). At the ~16 000-sample blocks §1.3
observes in practice rather than the 1 000 000 the buffers are built for, the
remaining ~111 MB would fall by a similar factor.

### 2.5 The same two patterns elsewhere

Both generalise, and the survey found them throughout. Applied:

    site                            what it runs at        before   after
    -----------------------------   --------------------   ------   -------
    RxVFOKernel post-resample       the VFO output rate,
      filter delay line               not the input rate
      - demod VFO, 2.4 M -> 250 k                           8.5 MB   1.35 MB
      - ctcss.ddc, 50 k -> 500                              8.5 MB   0.59 MB
    CTCSSSquelch ddcOut + fmOut     500 Hz (100:1 on NFM)   12.0 MB  0.12 MB
    IQFrontEnd reshape.out          one _nzFFTSize block    16.0 MB  1.05 MB
    7 decoder-module Reshapers      a fixed `keep`          8-16 MB  ~0.02 MB
    VOR amvOut + fmvOut + out       1 kHz (25:1)            24.0 MB  0.96 MB
    VOR amd -> magOut               an envelope buffer,
                                      not a demodulator     20.3 MB  4.00 MB
    POCSAG soft + out               2400 baud (10:1)        10.0 MB  1.00 MB

VOR and POCSAG are per-module and only pay when that decoder is loaded, but
they are the largest single reductions in the table: **39 MB** and **9 MB**.

The VOR one is not a sizing question at all. `amd` was a complete
`dsp::demod::AM<float>` -- its own output stream, a carrier-AGC buffer, a
`DCBlocker` and an LPF delay line -- that was never `start()`ed and never
`process()`ed. The receiver called `volk_32fc_magnitude_32f()` into
`amd.out.writeBuf` and used the result directly. It needed a 4 MB buffer and
was allocating a 20 MB demodulator to get one: the last instance of the
sibling-stream-as-scratch pattern the kernel split removed everywhere else.

The `RxVFOKernel` one is the most widely shared: its filter runs *after* the
resampler, so it sees the output rate, and every VFO in the application owns
one. `IQFrontEnd` can use `Reshaper::setKeepBound()` where `iq_exporter` cannot,
because `updateFFTPath()` holds a `TempStop` on both the reshaper and its sink
and so satisfies the precondition.

Not applied, and why:

* **RadioModule's AF chain.** CW interpolates 3 kHz -> 48 kHz while WFM
  decimates 250 kHz -> 48 kHz, and the demodulator changes at runtime under a
  chain that stays alive. Sizing it needs §1.3's analysis, not a ratio.
* **Every remaining `dsp::stream` whose size can still change**, including the
  VFO's own output. The consumer is a different block that the owner cannot
  stop, so `setBufferSize()` would free a buffer a running reader may hold.
  Streams whose bound is fixed for the life of the object *can* be sized at
  `init()`, which is what the VOR, POCSAG and RDS output streams above do; the
  rest is §7.1, and it is now the only thing between the current figures and
  another large reduction.
* **The Meteor demodulator**, where the arithmetic does not pay. It decimates
  only 150 kHz -> 72 kHz symbols, ~2:1, and `process()` uses `out.writeBuf` as
  its full-rate scratch for the RRC, AGC and Costas stages before the clock
  recovery narrows it. Shrinking `out` therefore means adding a full-rate
  scratch buffer of its own: 16 MB becomes 8 + 7.7, a 0.3 MB saving for a
  restructured `process()`. Left alone deliberately.

### 2.6 Rate-derived buffer sizes — review index

**Every allocation below used to be `STREAM_BUFFER_SIZE` and is now derived
from a sample-rate ratio.** Each is marked in the source with `[RATE-SIZED]`:

    grep -rn '\[RATE-SIZED\]' core/src decoder_modules

None of these buffers is bound-checked at the point of writing — that is §1 and
§7 item 1, and it has not changed. What *has* changed is that their capacity is
now an argument rather than a constant, so a wrong ratio is a silent heap
overflow instead of dead space. **This section is the list a human should
re-derive**, ideally against §7's capacity tracking once it exists.

Two kinds, and the distinction matters:

**Exact** — the ratio is structural, taken from something that also governs the
data path, so the bound cannot disagree with reality unless the code below it is
wrong:

    site                                      bound derived from
    ---------------------------------------   -------------------------------
    PowerDecimator::reconfigure() stageMax    the decimation plan's own
                                                per-stage ratios
    RationalResampler::reconfigure()          predecRatio, the power of two
      resampMax                                 the decimator above applies
    RationalResampler::maxOutputBlock()       predecRatio plus the selected
                                                resampInterp / resampDecim
    RxVFOKernel::filterMaxBlock()             RationalResampler's actual
                                                configured output bound
    Reshaper::init() maxKeep                  `keep`; bufferWorker() publishes
                                                exactly that many samples

**Assumed** — the ratio is computed from configured sample rates. If a rate
changes without the bound following it, or the resampler emits more than the
flat ratio predicts, these overflow:

    site                          ratio                        writes into
    ---------------------------   --------------------------   ------------------
    BroadcastFM::rdsBlockFor      RDS_SAMPLERATE / samplerate  rdsOut, and every
                                                                 buffer in RDSDemod
    CTCSSSquelch::decodeBlockFor  CTCSS_DECODE_SAMPLERATE /    ddcOut, fmOut
                                    samplerate
    vor::Receiver::outBlockFor    VOR_OUT_SR / VOR_IN_SR       amvOut, fmvOut, out
    POCSAGDSP::symbolBlockFor     baudrate / samplerate        soft, out

Each remaining assumed bound adds **64 samples of slack** over the flat ratio,
for the phase state a polyphase resampler or a clock-recovery loop retains
across blocks. That figure is a guess that has not been derived from the loops'
actual behaviour, and it is the single most likely thing in this section to be
wrong.

Three properties worth checking explicitly when this is reviewed:

1. **`RxVFOKernel::filterMaxBlock()` now follows the resampler's selected
   ratio**, including upward approximation and near-unity passthrough.
   `setInSamplerate()` and `setOutSamplerate()` both reconfigure the resampler
   before resizing the filter delay line.
2. **`BroadcastFM::rdsBlockFor()` leaves the object.** `getRDSMaxBlock()` hands
   it to `RDSDemod::init()`, which sizes two streams and four buffers from it.
   It is the only one of these bounds that crosses a module boundary, and
   nothing re-checks it on the other side.
3. **None of them clamps down when the ratio exceeds 1.** A VFO or resampler
   configured to interpolate produces *more* samples than it consumes, and
   clamping to `maxBlock` there would under-size rather than merely waste. The
   code deliberately returns the larger figure; §1.2 notes that nothing rejects
   such a configuration in the first place.

---

## 3. Producer block sizes

Maximum elements each producer writes into a `writeBuf`. These are **audited
upper bounds derived by reading the code at the current tree**, not measured
values, and for hardware sources they depend on the pinned dependency revision
and the device family — both of which should be recorded when a bound is
relied upon.

**This audit is incomplete.** See §4 for producers that are unbounded or
externally controlled, and §1 for why a complete source audit still would not
establish the invariant.

### 3.1 Fixed size — independent of sample rate

    source      max block   derivation
    ---------   ---------   ---------------------------------------------------
    hackrf        131 072   TRANSFER_BUFFER_SIZE 262144 B, valid_length/2
                            (hackrf_source/src/main.cpp:468)
    airspy         65 536   buffer_size 262144, FLOAT32_IQ -> /2 then /2;
                            packing off (libairspy airspy.c:997-1000)
    hydrasdr       65 536   DEFAULT_BUFFER_SIZE 262144, same math
    rtl_sdr         7 936   roundf(3.2e6/102400) = 31, x512 = 15872 BYTES
                            (main.cpp:351); callback does len/2 (:532)
    kcsdr           4 096   0x4000/4 (main.cpp:260). Note: the divisor is the
                            literal 4, not sizeof(complex_t) — 0x4000/8 would
                            be 2048 and is wrong.
    airspyhf        4 096   SAMPLES_TO_TRANSFER = 1024*4
    perseus         2 048   clamp(sr/200/1024, 1, 2) * 1024 — hard-capped
                            (perseus_source/src/main.cpp:272)
    qmxserver         512   EXT_BLOCKLEN (qmxserver_client.h:21), and the
                            packet length is checked against it before the
                            write (qmxserver_client.cpp:259)
    kiwisdr           512   onSamples(decoded, 512), fixed
                            (core/src/utils/proto/kiwisdr.cpp:104,117)
    qmx               512   qmx::kStreamBlockSize (QmxDevice.h:21)

`hackrf` is the largest fixed-size producer audited.

### 3.2 Sample-rate dependent — at each device's top rate

    source     formula          max rate       max block   rate source
    --------   --------------   ------------   ---------   --------------------
    usrp       sr/200           250 MS/s       1 250 000   UHD query (main:146)
    sddc       sr/100            64 MS/s         640 000   list caps (main:154)
    limesdr    sr/200          61.44 MS/s        307 200   LMS API query
    bladerf    sr/200, ->1024  see note          see note  srRange->max (:181)
    fobossdr   sr/400            50 MS/s         125 000   fobos_rx_get_
                                                           samplerates
    sdrplay    sr/200            10 MS/s          50 000   list caps (main:271)
    rfspace    sr/200 + one    1.2288 MS/s         ~6 200   default sr; list is
               packet                                      device-reported
    rtl_tcp    sr/200           3.2 MS/s          16 000   list caps (main:40)
    hermes     sr/200 + <=62    384 kS/s           1 982   list (hermes.cpp:5)
    audio      sr/200           192 kHz              960   RtAudio
    soapy      sr/200          device-dep.     unbounded   backend-defined
    network    sr/200 (TCP)    user-entered    unbounded   see §4

Note on **bladerf**: the rate range is read from the device at runtime
(`bladerf_get_sample_rate_range`), and some bladeRF2 configurations report above
61.44 MS/s. Do not treat any fixed figure here as a hard bound; it must be taken
from the pinned library and device family in use.

Note on **rfspace**: it accumulates into `&output->writeBuf[inBuffer]` until
`inBuffer >= blockSize` (`rfspace_client.cpp:241-246`), so it can overshoot
`blockSize` by up to one packet.

Buffer-size call sites: `usrp_source/src/main.cpp:410`,
`sddc_source/src/main.cpp:254`, `limesdr_source/src/main.cpp:430`,
`bladerf_source/src/main.cpp:331`, `fobossdr_source/src/main.cpp:296`,
`sdrplay_source/src/main.cpp:487`, `soapy_source/src/main.cpp:472`,
`rfspace_source/src/rfspace_client.cpp:136`,
`hermes_source/src/hermes.cpp:43`, `audio_source/src/main.cpp:187`.

### 3.3 Self-bounding — already clamp against the constant

These track `STREAM_BUFFER_SIZE` automatically and need no attention when it
changes. They are the reference pattern for §6.

    plutosdr        std::min<int>(sr/20, STREAM_BUFFER_SIZE)      main.cpp:462
    spectran        rejects pkt.num > STREAM_BUFFER_SIZE          main.cpp:419
    harogic         maxBufCount = STREAM_BUFFER_SIZE / bufferSize main.cpp:415
    rfnm            maxBufCount = STREAM_BUFFER_SIZE / sampCount  main.cpp:433
    file_source     clamp(sr/200, 1, STREAM_BUFFER_SIZE)          main.cpp:268
    network (UDP)   read buffer is STREAM_BUFFER_SIZE*sampleSize  main.cpp:265
    sdriak server   DECOMP_INPUT_BUFFER_SIZE derived from it,
                    payload size validated on receive             client.cpp:405

### 3.4 Not audited

    dragonlabs   callback `count` from libdlcr, which is not vendored in this
                 tree; device fixed at 12.5 MS/s. Must be closed before the
                 constant moves.

---

## 4. Unbounded and externally controlled producers

These are live defects, independent of any change to the constant.

**`network_source` TCP/SCTP is unbounded.** There are two paths
(`source_modules/network_source/src/main.cpp:262-265`). UDP sets
`forceSize = false` and reads into `sampleSize * STREAM_BUFFER_SIZE` — safe, and
listed in §3.3. TCP/SCTP sets `forceSize = true` and uses
`sampleSize * blockSize` with `blockSize = samplerate / 200`, where the
samplerate is typed into an `InputInt` clamped **only from below**
(`std::max<int>(tempSamplerate, 1000)`, `:238`). A user can enter any rate. At
the current constant this overflows above 200 MS/s; at 262 144 it would overflow
above ~52.43 MS/s.

**SpyServer does not validate `BodySize`.**
`source_modules/spyserver_source/src/spyserver_client.cpp:116` passes
`receivedHeader.BodySize` — taken straight off the wire — to
`readSize(..., readBuf)`, where `readBuf` is a fixed `new uint8_t[1 MiB]`
(`:10`) and `SPYSERVER_MAX_MESSAGE_BODY_SIZE` (`spyserver_protocol.h:19`) is
never compared against it. **This is a remote heap overflow reachable from a
hostile or malfunctioning server and is a memory-safety bug in its own right,
not a sizing issue.** Separately, even within the 1 MiB cap, `UINT8_IQ` yields
`BodySize / 2` = 524 288 complex samples (`:137`) — twice a 262 144 stream.

**QMX Server does *not* have this defect**, despite carrying a textual clone of
the SpyServer handler. That clone — including the unvalidated
`readSize(BodySize, readBuf)` and the IQ conversions — sits inside an `#if 0`
block (`qmxserver_client.cpp:131-220`) and is never compiled. The live data path
is the ENet receive at `:256-263`, which uses a fixed `EXT_BLOCKLEN` of 512 and
checks `event.packet->dataLength` against it before writing. It is listed in
§3.1 as one of the better-behaved producers. Do not "fix" the dead code.

**Spectran HTTP reads a server-supplied length into `writeBuf`.**
`spectran_http_client.cpp:143-152` sets `buf = (uint8_t*)stream->writeBuf` and
loops reading `clen - i` bytes into it, where `clen` is the HTTP content length,
with no comparison against stream capacity.

**`usrp_source` overflows at high rates, today, on 64-bit.**
`source_modules/usrp_source/src/main.cpp:410` computes
`int bufferSize = sampleRate / 200;` with no clamp and passes it to
`streamer->recv(stream.writeBuf, bufferSize, ...)`. UHD treats that argument as
caller-supplied capacity. At 200 MS/s it is exactly 1 000 000 — the whole buffer,
zero margin. An N320 at 250 MS/s writes 1 250 000 complex samples into a
1 000 000-element buffer.

**`soapy_source` is unbounded by construction.** `main.cpp:472` uses the same
`sr/200` with no clamp, passed to `readStream` as the per-buffer capacity; the
achievable rate depends entirely on the SoapySDR backend.

**`limesdr_source` has the same shape** (`main.cpp:430`, `sr/200` into
`LMS_RecvStream(&devStream, stream.writeBuf, sampCount, ...)`); it just does not
reach the limit at 61.44 MS/s.

---

## 5. The 32-bit / Android case

`android/app/build.gradle:73,85` builds `armeabi-v7a` alongside `arm64-v8a`. On
`armeabi-v7a`, an allocation fails during `MainWindow::init()` →
`ModuleManager::createInstance("Radio", …)` → `RadioModule::selectDemodByID()` →
`demod::WFM::init()`.

Historically this surfaced as a SIGSEGV inside `memset`, because
`dsp::buffer::alloc()` returned `volk_malloc`'s NULL unchecked and every caller
follows the allocation with `buffer::clear()`. All 47 `buffer::alloc` call sites
in the tree were unchecked. `alloc()` now reports the requested size and throws.

The FFTW allocations had the same defect and were missed by that pass:
`fftwf_malloc()` also returns NULL on failure, and its result went straight into
`fftwf_plan_dft_1d()`. All of them now go through `dsp::buffer::FFTWBuffer`
(§6.1), which checks. Note that `volk_malloc()` additionally returns NULL for a
**zero-sized** request, so `alloc(0)` now throws with a misleading
out-of-memory message; `FFTWBuffer::allocate()` rejects a non-positive count
with its own message instead. `taps::estimateTapCount()` returns
`3.8 * samplerate / transWidth`, which is 0 whenever the transition width
exceeds 3.8x the sample rate — not reachable from any current call site, but it
is the same shape as the non-positive-rate bug §1.4 had to fix.

### 5.1 OOM is fatal, by design

An allocation failure must not be swallowed and execution must not continue:

> A module constructor registers global state before it can fail, and when a
> constructor throws, the most-derived object's destructor does not run — only
> fully constructed member and base subobjects are destroyed.

`RadioModule` is the worked example. Before it reaches the demodulator it has
called `sigpath::vfoManager.createVFO()` (`radio_module.h:92`), bound a
waterfall handler carrying `this` (`:95`), and called
`sigpath::sinkManager.registerStream(name, &stream)` (`:128`). The matching
teardown lives in `~RadioModule`, which never runs. Continuing would leave
`sinkManager` holding a pointer into freed memory and the waterfall holding a
dangling `ctx` — a later use-after-free in place of a deterministic crash at
startup.

So the guarantee is only: *diagnosable and immediate*, not *recoverable*.

### 5.2 What recoverability would require first

DSP block initialisation was not exception-safe. The **leak-on-throw** half of
that is now fixed; the **consistency** half is not.

Fixed. Block destructors still bail on `if (!_block_init) { return; }` (e.g.
`core/src/dsp/demod/broadcast_fm.h:26`) and `_block_init` is still only set by
the `base_type::init(in)` call at the *end* of `init()` — but that no longer
matters for the allocations, because **member destructors run regardless of that
guard**, and even when the owning constructor itself throws. The raw-pointer
members this section used to list have owners (§6.1): `FIR::buffer`,
`Delay::buffer` and `RealToComplex::nullBuf` are `dsp::buffer::Buffer<T>`,
`BroadcastFM`'s `pilotFirTaps`/`audioFirTaps` are owning `dsp::tap<T>`, and the
`lmr`/`l`/`r` buffers are scratch `Buffer<float>`. An `init()` that throws
part-way now releases what it had taken, at the moment memory is scarcest.

Still open, and each of these has to be closed before an OOM could be *caught*
rather than merely diagnosed:

* **A failed reconfiguration restarts the block anyway.** `block::TempStop`
  always calls `tempStart()` in its destructor, including while the exception
  that aborted the reconfiguration is still unwinding, so the worker resumes
  against whatever half-updated state the throw left. `FMIF::setBins()` is the
  worked example: `initBuffers()` releases everything and rethrows, and
  `process()` has to check `bufferStart` and pass through, or the restarted
  worker faults before the diagnostic reaches the log. Making the guard *not*
  restart is not the fix — skipping `tempStart()` leaves `tempStopDepth`
  elevated, and the block then never restarts at all.
* **Composites mutate sub-objects one at a time.** `RationalResampler::
  reconfigure()` handles this by dropping to `Mode::NONE` up front and
  re-committing the mode at each exit, and `PowerDecimator::reconfigure()` and
  `MM::setInterpParams()` build replacements into locals before committing. That
  pattern is not applied everywhere — `BroadcastFM::setSamplerate()` still walks
  its sub-objects in sequence with no rollback.
* **A few raw allocations remain** in the radio graph: `PowerSquelch::
  normBuffer` and `dsp::buffer::RingBuffer::_buffer` are still raw pointers
  freed only under the `_block_init` / `_init` guard, so each has a narrow
  leak window if `init()` throws after the allocation.

None of this changes §5.1: OOM is still fatal by design, because a module
constructor registers global state before it can fail.

### 5.3 Lowering the constant — preliminary figures only

**These numbers do not constitute a safety case.** §3 is incomplete and §4 lists
producers that are unbounded regardless of the constant. §7 is the gate.

Among sources both built **and loaded** by Android's hard-coded module list, the
largest audited fixed-size producer is hackrf at 131 072. SDDC is
`OPT_BUILD_SDDC_SOURCE=OFF` by default, USRP needs UHD, and
BladeRF/LimeSDR/FobosSDR are not in the Android build. Network Source and
Spectran HTTP are compiled for Android but absent from the hard-coded list in
`core/src/core.cpp`, so normal Android users cannot load them. Dragonlabs is
also absent from that list.

SpyServer is loaded and remains externally sized until its `BodySize` is
validated and fragmented. The internal FFT and interpolation findings in §1
also mean that "nothing exceeds 131 072" cannot be established from the source
table alone. See §7.1 for the order in which this should be approached.

    candidate   vs. audited hackrf   constructed   retained = peak
    ---------   ------------------   -----------   ---------------
    1 000 000   7.6x                       50 MB           111 MB
      262 144   2.0x                       13 MB            35 MB
      131 072   1.0x  (no margin)           7 MB            22 MB
       65 536   0.5x  (must fragment)       3 MB            15 MB

Before the kernel split: 284/342, 74/91, 37/47, 19/25. After it but before
rate-aware sizing: 50/199, 13/58, 7/33, 3/21. "Peak" and "retained" are one
column because initialisation no longer overshoots its end state (§2.2).

Recomputed per value rather than scaled from the 1 000 000 row, as
`103 x capacity + 8.35 MB`. The 103 bytes per element break down as 16.5 of
streams, 48.3 of full-rate scratch, 28 of full-rate FIR and delay lines, 9.8 of
decimator stages and the polyphase delay line, and **0.3 for the entire RDS
branch** — which contributed 62 before §2.4. Note the `constructed` column is
unchanged: a `dsp::stream` is built at `STREAM_BUFFER_SIZE` by its constructor
and only narrowed by `setBufferSize()` during `init()`, so rate-aware sizing
shows up in the retained column, not that one. **The 8.35 MB does not scale**,
and it is the larger share at every candidate below 131 072:

* the `+ 64000` in the `FIR`, `Delay` and `PolyphaseResampler` working buffers
  (`core/src/dsp/filter/fir.h:30`, `core/src/dsp/math/delay.h:28`) contributes
  4.35 MB regardless of the constant — at 131 072 it is roughly half of each
  such buffer; and
* `Reshaper`'s `RingBuffer<float>` is sized by its **own** literal,
  `RING_BUF_SZ = 1000000` (`core/src/dsp/buffer/ring_buffer.h:4`), which is not
  `STREAM_BUFFER_SIZE` and does not move with it. That is 4 MB that lowering the
  constant would not touch, and it should be tied to the destination or made a
  parameter alongside the `maxBlock` work in §1.1.

At 65 536 those fixed adders are **55 %** of the WFM total — up from 40 %
before §2.4, because rate-aware sizing removed scaling bytes and left the fixed
ones untouched. That is why the bottom two rows barely separate, and why §7
item 9 matters more after this change than before it. The figures are WFM-only;
see §2.3.

The polyphase taps and phase banks (§1.4, §2.1) are **not** in these columns and
do not scale with the constant at all — they scale with the resampling ratio. At
the current ratios they total 0.875 MB across the radio graph, so they no longer
matter; before the §1.4 fix they were 16.28 MB, which would have dominated the
262 144 and 131 072 rows.

Lowering the constant on **all** platforms would additionally require clamping
usrp, limesdr, bladerf, sddc and soapy (§4).

---

## 6. Rule for new producers

A producer must never assume the constant's value. Either:

* derive the block size from `STREAM_BUFFER_SIZE` and clamp against it, as
  `plutosdr` (`main.cpp:462`) and `file_source` (`main.cpp:268`) do; or
* validate a library- or peer-supplied count before copying, as `spectran`
  (`main.cpp:419`) and `qmxserver` (`qmxserver_client.cpp:259`) do.

**This rule is transitional.** Clamping against the global `STREAM_BUFFER_SIZE`
is only correct while every stream is that size. Once §7.1 lands — per-stream
capacity, updated by `setBufferSize()` — a producer must clamp against *the
destination stream's actual capacity*, not the global default, or it will
overflow any stream that was resized. `sample_stream_compressor.h:17` and
`sdriak_server_client.cpp:79` already call `setBufferSize()`, so streams of
non-default size exist today.

`sr/200` written bare is the bug pattern in §4. It is safe only while the rate is
known and small, and that assumption dies the moment the rate list, the hardware,
or a user-editable field changes.

Any block whose output count can exceed its input count (§1) must validate
output capacity before writing, not after.

### 6.1 Ownership: use an owner, not a raw pointer

Every allocation in the DSP graph now has an owner type. New code uses one; the
raw-pointer-plus-destructor pattern is what §5.2 had to unwind.

    owner                                allocator          for
    ----------------------------------   ----------------   ---------------------
    dsp::buffer::Buffer<T>               volk_malloc        SIMD scratch and
                                                            working buffers
    dsp::buffer::HistoryBuffer<T>        volk_malloc        a delay line: the
                                                            last H samples plus
                                                            one block <= maxBlock
    dsp::tap<T>                          volk_malloc        a generated tap set
    dsp::multirate::PolyphaseBank<T>     volk_malloc        a phase bank
    dsp::buffer::FFTWBuffer<T>           fftwf_malloc       FFT working buffers
    dsp::buffer::FFTWPlan                fftwf_plan_*       an FFT plan

Four properties they share, each of which was a defect before:

* **They free on the throw path.** Member destructors run even when the owning
  `init()` or constructor throws, which is what §5.2 turns on.
* **`allocate()` acquires before it releases.** A failed reallocation leaves the
  owner on its existing storage rather than empty, so a caller that catches is
  not left with a dangling derived pointer. Recompute any `bufStart`-style
  pointer after it returns.
* **They are move-only, with the move operations declared explicitly.**
  Declaring copy operations suppresses the implicit moves. `tap<T>` and
  `PolyphaseBank<T>` used to be plain structs with separate `Owned*` wrappers
  deriving from them; they are now the owners themselves, so there is no
  plain-struct `operator=` left for a `std::move` to bind to by mistake.
  The FIR family (`FIR`, `DecimatingFIR`, `FIRBlock`) owns the taps it is
  given as an rvalue — every call site in the tree passes a generator result
  or `std::move` — and frees the previous set only after the replacement is
  published. An lvalue is borrowed instead, for a set shared with something
  else; the FIR tracks which case it is in. `PolyphaseResampler` copies the
  taps into its phase bank and keeps no reference, so its callers hold none
  either.
* **The allocator is part of the type.** `FFTWBuffer` exists separately from
  `Buffer` because an FFT plan records the *alignment* of the arrays it was
  built against: reallocating a buffer invalidates the plan, so
  `FFTWPlan::planDFT1D()` makes re-planning one step rather than something to
  remember. `FFTWPlan` also holds the process-wide planner lock across every
  FFTW call, since only `fftwf_execute()` is thread-safe and this tree plans
  from the UI, a source thread and a DSP worker.

Two mechanical consequences at call sites. `Buffer`/`FFTWBuffer` convert
implicitly to `T*`, but a **C-style cast cannot see through a user conversion**,
so `(lv_32fc_t*)buf` must become `(lv_32fc_t*)buf.get()` — a compile error, not a
silent bug. And template argument deduction does not consider user conversions
either, so `buffer::clear(buf, n)` must name the type: `buffer::clear<T>(buf, n)`.

Ownership must also be declared **before** whatever reads it, since members are
destroyed in reverse order. With the FIRs owning their taps this no longer
applies to tap sets; it still applies to any borrowed set a future caller
hands a FIR by lvalue.

---

## 7. What must land before the constant changes

1. **Capacity tracking in `dsp::stream`.** Store the allocated capacity, update
   it in `setBufferSize()`, expose a query, and provide a bounded-write or
   reservation API. Keep a `swap()` check as secondary corruption detection, not
   as the primary guard — it runs after the write.
2. **Phase-aware pre-write handling in expanding blocks** —
   `PolyphaseResampler`, `RationalResampler`, `RRCInterpolator`, `RxVFO` and any
   other block whose output count can exceed its input count. Predict the output
   count before writing and fragment the input/output when it does not fit. The
   kernels now accept a `maxBlock` bound on their *input* (§1.1), which is the
   argument this item needs to be told about the destination; nothing passes a
   non-default value yet, and the output side is still unchecked.
3. ~~**Bound rational ratios and validate rates.**~~ **DONE.** The ratio is
   derived exactly in 64-bit and the phase count is capped at 512 by continued
   fraction, and non-positive rates are rejected before the GCD (§1.4).
4. **Test the RRC interpolator.** The two defects are fixed — `_rrcBeta` is
   assigned before `genTaps()` and the taps no longer outlive their use — but they were
   reasoned from the code, not caught by a test. Output counts and
   reconfiguration are still uncovered (§1.5).
5. **Bound or fragment the §4 producers** — network (TCP), SpyServer,
   Spectran HTTP, USRP, Soapy, LimeSDR. The SpyServer `BodySize` validation is a
   memory-safety fix and should be handled on its own, ahead of this work.
6. **Audit §3.4** — dragonlabs.
7. **Tally whole-process construction**, especially RadioModule's own members
   (§2.3), not just the demodulator. This is now the weakest number in the
   document: §2.1 and §2.2 are line-by-line against the current member lists,
   §2.3 is still an estimate.
8. **Reproduce on the affected device**, recording ABI, the failing allocation
   size, native allocation profile, RSS/PSS and `/proc/<pid>/maps`. The failure
   depends on existing mappings, fragmentation and device limits, and none of
   that has been measured. The kernel split (§2.1) cut the WFM construction peak
   from ~342 MB to ~199 MB, so the original `armeabi-v7a` crash may no longer
   reproduce — **that would not close this item.** It would move the failure
   threshold, not establish one, and the retained figure is still ~199 MB for a
   single demodulator.
9. **Fix the constants that do not track `STREAM_BUFFER_SIZE`** — the
   `+ 64000` working-buffer headroom and `RING_BUF_SZ` (§5.3). They are 8.35 MB
   of the WFM total at any capacity, which is 55 % of it at 65 536.
11. **Re-derive every `[RATE-SIZED]` bound** (§2.6), in particular the 64-sample
   slack each of them adds for retained resampler and clock-recovery phase.
   These are the allocations that stopped being constant, and none of them is
   bound-checked at the point of writing.
10. **Make a failed reconfiguration leave a coherent block** (§5.2): `TempStop`
   restarts the worker while the exception is still unwinding, so every
   reconfiguration has to be transactional -- acquire everything into locals,
   commit with no-throw moves. Applied so far in `FIR::setTaps()`,
   `PolyphaseResampler::setRatio()`, `CTCSSSquelch::setSamplerate()`,
   `RxVFOKernel` (the tap sets), `Splitter::bindStream()`, `IQFrontEnd::
   addVFO()` and `IQFrontEnd::updateFFTPath()`, which now computes `_fftSize`,
   `_nzFFTSize`, the keep and the skip into locals and commits them with the
   window, the FFT buffers and the plan. `Reshaper::setKeepBound()` is the one
   throwing step left above that commit point, and deliberately so: growing the
   bound `out` is sized for never moves the block size the reshaper emits, so a
   later throw leaves nothing behind but a roomier stream. Shrinking it is the
   mirror image and runs below the commit point, where a failure likewise only
   costs memory -- it is logged, not propagated.
   **Still open**: `BroadcastFM::setSamplerate()` walks its sub-objects with no
   rollback.

### 7.1 Staging

The chosen direction is to preserve the Android source-module set and make
large producers and expanding DSP blocks capacity-aware. Module removal is not
a substitute for the missing capacity contract.

    stage   action                                         depends on
    -----   --------------------------------------------   ----------
    now     validate SpyServer BodySize and publish it in
            capacity-safe chunks                           nothing here
    next    add per-stream capacity and secondary swap()
            assertions                                     —
    then    make resamplers phase-aware and fragment their
            output; test RRC                               1, 2
            (bounding the rational ratio is DONE, §1.4;
             maxBlock plumbing exists but is unused)
    then    protect the IQ FFT reshaper and fragment fixed
            callbacks such as HackRF where required         1
    then    parameterise RING_BUF_SZ and the +64000
            headroom so they track the destination          1, 9
    last    lower the 32-bit constant and verify on-device,
            first at 131 072 and then evaluate 65 536      1-9

At 131 072, HackRF exactly fills a stream and has no margin. At 65 536 it must
be split in two, while Airspy and HydraSDR exactly fill the stream. Values below
65 536 require progressively more source fragmentation. Section 1.3 shows that
the live radio interpolation path does not by itself justify 262 144, but it
must still be guarded before any smaller value becomes a correctness claim.
