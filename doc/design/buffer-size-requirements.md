# Buffer size requirements — `STREAM_BUFFER_SIZE` and producer block sizes

Read from the tree of 2026-11-15. Figures are counted from the code, not
measured.

Status: **preliminary diagnosis — not a completed requirements analysis.** The
capacity audit in §3 is not complete enough to justify changing
`STREAM_BUFFER_SIZE`, and §7 lists what must land first. Do not use §5.3's
table as a safety case.

**For a short summary, read `buffer-size-requirements-current.md` first.**
This document is the detailed reference behind it: the use-site inventory, the
allocation traces, the index of rate-derived bounds and the producer audit.

`STREAM_BUFFER_SIZE` (`core/src/dsp/stream.h`) is 1 000 000 elements, and
262 144 on 32-bit Android. It looks like a tuning knob. It is not: it is the
implicit capacity contract between every producer in the DSP graph and every
stream it writes into, and it is a large part of the application's native
memory footprint. The WFM figures below are at 1 000 000.

Compared with upstream SDR++, where every DSP stage is a block with its own
output stream and every buffer is sized for a full block, the WFM demodulator
constructs 34 MB instead of 284 MB and peaks at ~86 MB instead of ~342 MB, and
a radio session at its startup peak needs ~294 MB instead of ~663 MB.

See `doc/design/dsp-pipeline.md` for how streams and blocks fit together; this
document only covers sizing.

---

## 1. The constant is a default, not an enforced invariant

A `dsp::stream<T>` allocates two buffers of `STREAM_BUFFER_SIZE` elements in its
**constructor** (`core/src/dsp/stream.h`). A producer writes into
`stream.writeBuf` and only afterwards calls `swap(n)` to publish `n` samples.

The stream records its capacity (`capacity()`), and `swap(n)` aborts with a
logged message when `n` exceeds it. But `swap(n)` runs *after* the write, so
that check detects corruption rather than preventing it.

> If any producer writes more than the stream's capacity into a `writeBuf`, the
> result is a heap overflow. The `swap()` check makes it loud instead of
> silent, but only for writes that are then published; scratch and workspace
> buffers have no check at all.

Two consequences shape the rest of this document.

**Source maxima cannot prove the invariant.** §3 audits what each source module
produces, and that is necessary but not sufficient, because sources are not the
only producers.

**Internal blocks can expand, and shipped chains do.** Every streaming
interpolator shares one unchecked write (§1.1), and several live paths expand.

### 1.1 The shared resampler defect

All rational and RRC resampling reaches `PolyphaseResampler::process()`
(`core/src/dsp/multirate/polyphase_resampler.h`). It performs two writes that
release builds do not check:

* `hist.push(in, count)` copies the input in behind the retained history, in a
  `HistoryBuffer` of exactly `maxBlock + tapsPerPhase - 1` elements. `push()`
  asserts `count <= maxBlock`, in debug builds only; and
* the convolution loop appends to `out[outCount++]` without receiving or
  checking an output capacity.

For interpolation ratio `L/M`, the number of output elements is approximately
`ceil(count * L / M)`, with the exact count also depending on the retained
polyphase state. `process()` returns `outCount` only after it has written the
result, so checking `swap(outCount)` cannot prevent an overflow.

`RationalResampler` wraps this implementation. Its downsampling modes do not
expand a valid input block and are therefore bounded if both streams have the
same capacity. `RESAMP_ONLY`, used for upsampling, has no such property.
`RRCInterpolator` delegates directly to the same `process()` implementation.
The delay line holds exactly `maxBlock + history`, so an oversized input block
overruns it immediately, and in a release build nothing reports it.

The fused kernels (`FIR`, `Delay`, `PolyphaseResampler`, `PowerDecimator`,
`RationalResampler`, `RxVFOKernel`, `MM`) take `maxBlock` at `init()` instead of
reading `STREAM_BUFFER_SIZE` directly. The resampling ones also publish
`maxOutputBlock()` and `maxOutputBuffer()`, with static previews for rates not
yet applied (§2.6), so a composite can size whatever sits behind a kernel from
what that kernel will actually emit. **Every top-level entry point uses the
default**, so every bound in §2.6 is derived from `STREAM_BUFFER_SIZE`, even
though streams report `capacity()`. The bounds *predict* the output. Nothing
compares the prediction with the caller's actual allocation before it writes;
`swap()` catches an oversized stream write only afterwards. That is the
unchecked half of the defect.

### 1.2 Use-site inventory

The tree contains these resampler users:

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
    WebSDR small waterfall        12 kHz -> 12 kHz                    vector sized from the plan; bounded
    modulator GFSK/PSK            symbols -> baseband                 no call site

All calls to `vfoManager.createVFO()` use `RxVFO` and therefore inherit its
resampler. The call sites are Radio, M17, Meteor, Radiosonde, Weather Satellite,
VOR, RyFi, KG-SSTV, Falcon 9, DAB, Pager, ATV and IQ Exporter. The intended case
is decimation, but neither `IQFrontEnd::addVFO()` nor `RxVFO` rejects a requested
output rate above the current source rate. In that case the VFO becomes another
unchecked interpolator. This matters most for arbitrary or low source rates.

The `rational_resampler.h` includes in DAB and Pager do not instantiate a
resampler.

On Android, the hard-coded loader includes the live resampler users M17,
Meteor, Radio, Radiosonde and WebSDR View. ATV, Falcon 9, DAB and the other
decoder call sites above are not loaded there.

### 1.3 Radio AF interpolation: code bound versus operational bound

The radio audio-chain resampler runs from the demodulator's AF rate to whatever
rate the sink reports (`radio_module.h`). At a 192 kHz sink:

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
source callback is multiplied by 64. The callback first passes through the VFO,
which converts the source rate to 3 kHz. For a continuous chain the two ratios
approximately cancel:

    source block * (3 kHz / source rate) * (192 kHz / 3 kHz)
      = source block * (192 kHz / source rate)

For example, a 131 072-sample HackRF callback at 2 MHz becomes about 12 583
audio samples at 192 kHz, and a 512-sample KiwiSDR block at 12 kHz becomes
8 192. Both fit a 65 536-element stream. This explains why the 64x path works
and weakens it as evidence for choosing 262 144 over 65 536. It does not
establish safety: no code enforces a maximum block duration, and a large
low-rate network block can still expand past the destination.

M17 is also operationally bounded: its Codec2 decoder publishes
`2 * codec2_samples_per_frame()` samples at 8 kHz, approximately 320 in the
configured mode, so even 24x conversion to 192 kHz produces only about 7 680
samples. Radiosonde is less well specified: its 10 kHz mode interpolates 4.8x
to 48 kHz and relies on the VFO block feeding it being small; there is no
explicit limit.

Chunking a source only to the immediate stream capacity is not sufficient when
a downstream VFO or AF stage can interpolate it. A correct chunk limit must
also account for downstream expansion, or the expanding block itself must
fragment its output.

### 1.4 Rational-rate allocation bound

`RationalResampler::reconfigure()` derives the ratio exactly, as
`_outSamplerate * predecRatio` over `_inSamplerate` in 64-bit, and caps the
phase count at `MAX_INTERP = 512` using the convergents of the continued
fraction expansion plus the best semiconvergent (`approxRatio()`). The cap is
on `interp` **only**: `decim` is just the stride of the phase accumulator and
costs no memory, and bounding it would spoil exact ratios such as 384/625.

Deriving the ratio from rounded rates would not work: the predecimated rate is
usually fractional (2 500 000 / 64 = 39 062.5), and rounding destroys the
common factor — `gcd(39063, 24000)` is 3 where the true ratio reduces to
384/625, which would mean an 8 000-phase bank. The hardware paths therefore
have a rate error of exactly zero, and only genuinely coprime pairs are
approximated: a 44 101 Hz file to 48 kHz gives 357/328, 0.00036 % error.

Phase banks at the current ratios, one allocation each (§6.1):

    instance                       context               taps + bank
    ----------------------------   -------------------   -----------
    BroadcastFM rdsResamp          WFM, fixed rates        15.3 KB
    RadioModule afChain resamp     WFM AF -> 48k             76 KB
    CTCSSSquelch ddc.resamp        NFM AF -> CTCSS 500     15.3 KB
    RxVFO, RTL 2.4M                -> WFM IF                3.7 KB
    RxVFO, Airspy 2.5M             -> USB IF                382 KB
    RxVFO, Airspy 2.5M             -> CW IF                 382 KB
                                                         ----------
                                                          0.875 MB

Non-positive rates are rejected before the GCD, since they would reach
`buildPolyphaseBank()` as a zero phase count; `reconfigure()` stays in
`Mode::NONE`. Network Source accepts arbitrary integer sample rates (§4), which
is a block-size question, not an allocation one.

### 1.5 Test coverage

`rational_resampler_tests` exercises output counts against the configured bound
across retained phase state, including decimation, interpolation, fractional
conversion, and the near-unity approximation that can select passthrough. It
puts a canary one element past `maxOutputBuffer()` as well as past
`maxOutputBlock()`, checks that the static previews agree with a configured
resampler, and pins the production RDS and CTCSS bounds, including the two
configurations where a flat ratio estimate is short (§2.6).
`RationalResampler::Mode::NONE` skips the copy when `out == in`, which the
fused `RxVFOKernel` relies on.

    suite                     covers
    ------------------------  ---------------------------------------------------
    clock_recovery_mm_tests   MM::maxOutputBlock() for POCSAG, its tracking of
                              setMuGain(), a canary run with the loop held at its
                              lower frequency limit, and rejection of parameters
                              with no finite bound
    fir_history_tests         FIR capacity changes keep history; RxVFOKernel
                              keeps it across an input-rate change and clears it
                              on an output-rate change (§2.6 property 5)
    fftw_lifetime_tests       a static FFTW plan outliving the planner lock (§6.1)
    block_restart_tests       a doStart() that throws part-way leaves the block
                              stopped, joinable workers joined, and restartable
                              (§5.2)
    frame_tap_tests           FrameTap frame boundaries at any block size, for
                              contiguous, sliding and decimating geometries

All of them are built only with `OPT_BUILD_TESTS` and never on Android. Not
covered: large interpolation ratios, the `RationalResampler` output bound after
a live rate change (its preview is tested, the reconfigured instance is not),
and the RRC interpolator. Those tests are required before a smaller global
capacity can be treated as safe.

The modules in §3.3 clamp against the constant or the stream's capacity
themselves and so track it automatically. Everything else in §3.1, §3.2 and §4
does not.

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

### 2.1 Construction: kernels and blocks

Most DSP classes are plain **computation kernels** — no output stream, no worker
thread, no mutexes — whose `process()` the containing composite calls inline.
Only **blocks** own an output stream.

    kernels (no stream)         FIR, DecimatingFIR, Delay, FrequencyXlator,
                                RxVFOKernel, Quadrature, RealToComplex, AGC,
                                FastAGC, PLL, CarrierTrackingPLL, Costas, MM,
                                DifferentialDecoder, PolyphaseResampler,
                                PowerDecimator, RationalResampler,
                                RRCInterpolator, MeteorCostas

    blocks (one stream)         the demod composites (AM, FM, BroadcastFM, SSB,
                                CW, GFSK, PSK), DCBlocker, Deemphasis,
                                NoiseBlanker, PowerSquelch, FMIF, CTCSSSquelch,
                                Splitter, SampleFrameBuffer, and the
                                threaded adapters FIRBlock, FastAGCBlock,
                                PowerDecimatorBlock, RationalResamplerBlock,
                                RxVFO

    neither                     dsp::buffer::FrameTap: a 2 x keep history the
                                producer fills inline, for diagnostic displays
                                and fixed integration windows

Twelve of the fourteen `filter::FIR` instances in the tree are fused; the two
that are chain nodes (`RadioModule::hpf`, the RyFi transmitter's `rrc`) use
`FIRBlock`. The VFO kernel is `RxVFOKernel` because `RxVFO` is public API
(`vfo_manager.h`, `iq_frontend.h`).

**The counting rule: count blocks, not sub-objects.** A kernel member costs
nothing at construction; only its `init()` allocates, and only a working
buffer.

    dsp::demod::BroadcastFM            2 streams         32 MB
      8-byte  (2): out(stereo), rdsOut
      (demod, pilotFir, rtoc, xlator, pilotPLL, lprDelay,
       lmrDelay, alFir, arFir and rdsResamp are all kernels)
    RDSDemod                           1 stream           2 MB
      1-byte  (1): out
      (agc, costas, fir, costas2, recov, diff are all kernels;
       the soft symbols are a private buffer, allocated in init())
                                                      -------
    constructed by `new demod::WFM()`                    34 MB

The `sink::Handler` member `hs` derives from `Sink`, which has no output
stream, and costs nothing; the diagnostic `softTap` is a `FrameTap`, allocated
in `init()`.

This counts **streams only**. Three other kinds of allocation are not in the
figure above:

* FIR, delay and resampler delay lines: a `HistoryBuffer` of exactly
  `maxBlock + history`, where the history is the tap count, or the delay, less
  one — a few hundred elements at most. They are in the §2.2 trace.
* **Scratch buffers.** A fused kernel needs somewhere to put its output, so the
  composites allocate `dsp::buffer::Buffer<T>` scratch in `init()`: one buffer
  rather than a stream's two. BroadcastFM allocates 28 bytes per element across
  five of them, because every stage that can runs in place on a buffer an
  earlier stage owns. RDSDemod allocates one complex and one float buffer, at
  the RDS rate.
* Polyphase filter taps and the phase bank. These scale with the *resampling
  ratio*, not with `STREAM_BUFFER_SIZE` (§1.4). `rdsResamp` holds 15.3 KB.

### 2.2 Peak during initialisation

`WFM::init()` allocates delay lines, scratch and taps. The trace, in MB, at the
250 kHz WFM IF rate and the desktop constant of 1 000 000. `rdsResamp.init()`
starts both stages empty, so `reconfigure()` builds only what `plan_32` uses.

    after construction                                      34.0
    + pilotFir 8.0, lprDelay 4.0, lmrDelay 8.0,
      alFir 4.0, arFir 4.0                                  62.0
    + rdsResamp.init() -> reconfigure(): plan_32 stages
      8.0 + 1.0 + 0.5, polyphase delay line 0.25            71.8
    + rdsOut.setBufferSize(125 000): 2.0 replaces 16.0      57.8
    + BroadcastFM scratch (demodOut 4, rtocOut 8,
      pilotOut 8, lmr 4, l 4)                               85.8
    + RDSDemod::init: fir 0.16, recov 0.08, scratch 0.16,
      soft buffer 0.08, new out 0.04          transient     86.3   <-- peak
      out narrowed (2 -> 0.04)                              84.3
    + softTap.init: 2 x 4 096 floats 0.03                   84.4   retained

250 kHz -> 5 kHz gives `predecRatio` 32, so the plan is `plans[4]` = `plan_32`.
Its stages decimate by {8, 2, 2} (`core/src/dsp/multirate/decim/plans.h`), so
they are sized for inputs of 1 000 000, 125 000 and 62 500 samples.

**The peak overshoots the end state by 2 MB.** `rdsOut` (16 → 2.0) and
RDSDemod's `out` (2 → 0.04) are constructed at `STREAM_BUFFER_SIZE` and shrunk
only in their owners' `init()`; BroadcastFM's scratch is allocated while
RDSDemod's `out` is still full-size. Building those streams at their final size
would bring the peak down to the retained 84.4. A stream's constructor
allocates `STREAM_BUFFER_SIZE` unconditionally, so that needs streams that can
start without buffers (`buffer-size-requirements-current.md` §5.1 step 3).

**34 MB constructed, ~86 MB peak, ~84 MB retained — for one demodulator**,
in 2–16 MB chunks needing contiguous virtual address space. The retained
84 MB is 28 MB of full-rate delay lines, 28 MB of full-rate scratch, 16 MB of
output stream, 9.8 MB of RDS decimator stages and 2.6 MB of RDS branch. That is
what §5.3 has to move.

### 2.3 This is not the process peak

Two things the numbers above deliberately exclude.

**RadioModule allocates more than the demodulator, and allocates first.** Its own
members are constructed before `instantiateDemod()` is called
(`decoder_modules/radio/src/radio_module.h`, member block at the end of the
class). Counting blocks, not sub-objects:

    nb, fmnr, powerSquelch, dummyAudioStream       4 streams     64 MB
    ctcss  (CTCSSSquelch)                          1 stream      16 MB
      its own out (stereo); ddc is an RxVFOKernel
      and fm a Quadrature kernel, so neither adds one
    resamp (RationalResamplerBlock<stereo_t>)      1 stream      16 MB
    hpf (FIRBlock<stereo_t,float>), deemp          2 streams     32 MB
    SinkManager::Stream: volumeInput, volumeAjust  2 streams     32 MB
                                                 -----------   -------
    10 x 8-byte                                   10 streams    160 MB

`CTCSSSquelch` adds two scratch buffers (`ddcOut` complex, `fmOut` float).
`dsp::chain` and `routing::Splitter` hold only pointers and cost nothing.

Its init-time allocations, in the configuration the constructor leaves them
in. `selectDemodByID()` retunes them only after `WFM::init()` has run, so this
is also the state the WFM peak lands on: CTCSS at the 50 kHz NFM AF rate
(500 Hz decode, a 100:1 drop across `ctcss.ddc`), and the AF resampler at
250 kHz -> 48 kHz.

    fmnr (FMIF, 32 bins)       delay line                    8.0 MB
    ctcss.ddc                  plan_64 stages 8.0 + 1.0 +
                               0.25                          9.3 MB
                               polyphase delay line          0.13 MB
                               post-resample filter
                               delay line                    0.08 MB
    ctcss ddcOut               full-rate xlator output,
                               then decimator workspace      8.0 MB
    ctcss fmOut                500 Hz                        0.04 MB
    resamp (250 kHz -> 48 kHz) plan_4 stages 8.0 + 4.0,
                               polyphase delay line 2.0     14.0 MB
    hpf                        delay line                    8.0 MB
    nb, powerSquelch, deemp                                  0.0 MB
                                                           --------
                                                            47.5 MB

    member construction (10 streams, above)                 160 MB
    retained after RadioModule DSP init, also its peak     207.5 MB
    retained + the WFM peak from §2.2 (207.5 + 86.3)        294 MB
    after WFM init completes (207.5 + 84.4)                 292 MB

When `selectDemodByID()` then moves CTCSS to WFM's 250 kHz AF rate, its
decimator switches to `plan_256` and the total drops by about 1 MB. The AF
resampler is already at 250 kHz.

**The 160 MB of streams is three quarters of it.** The init line is mostly
full-rate delay lines plus `ddcOut`, and shrinks only when the block bound does
(§7 items 1–2). The ten streams are all full-capacity. The VFO's own output
stream is the clearest case: it converts the source rate to 250 kHz, so it
carries about a tenth of what it is built for. It is blocked on §7.1 for the
reason §2.4 gives: a stream's consumer is a different block, and the owner
cannot stop it to resize safely.

The 294 MB line is the closest thing to a process peak in this document. It
excludes the VFO created by `vfoManager.createVFO()`, the source module, the
front end, waterfall history, other modules and allocator overhead. §7 item 5
asks for a real tally.

**Requested bytes are not resident bytes.** Everything here counts allocation
requests. Physical footprint (RSS/PSS), address-space fragmentation and
allocator overhead are separate quantities and have not been measured. On
32-bit, fragmentation of the 4 GB address space is often what actually fails a
large contiguous request, so whether a given figure fits is a question for
on-device measurement (§7).

### 2.4 Rate-aware sizing

Every buffer is sized for **the sample rate its own branch runs at**, where that
rate is known at `init()`/`reconfigure()` time. The RDS branch, which runs at
1/50 of the WFM IF rate, is the main case:

    buffer                         sized for                       size     at full capacity
    -----------------------------  ------------------------------  -------  ----------------
    rdsResamp decimator stages     cumulative decimation {8,2,2}   9.5 MB   24 MB
    rdsResamp polyphase line       its post-decimation input       0.25 MB  8 MB
    rdsOut (stream)                decimator workspace, 125 000    2.0 MB   16 MB
    RDSDemod scratch (complex)     final RDS block, 20 001         0.16 MB  8 MB
    RDSDemod soft buffer (float)   final RDS block                 0.08 MB  4 MB
    RDSDemod FIR delay line        final RDS block + taps          0.16 MB  8 MB
    RDSDemod clock-recovery line   final RDS block + taps          0.08 MB  4 MB
    RDSDemod out (stream)          final RDS block                 0.04 MB  2 MB

`BroadcastFM::rdsResamp` is 250 kHz -> 5 kHz, so it publishes at most 20 001
samples from a one-million-sample input block, including retained phase. Its
destination is also working storage: the first stage of the 32:1 power
decimator writes 125 000 intermediate samples there before later stages reduce
them, so `rdsOut` holds 125 000 while the count it swaps is at most 20 001.
`BroadcastFM::getRDSMaxBlock()` publishes the final bound and
`RDSDemod::init()` takes it.

`RealToComplex` writes its zero imaginary part directly rather than interleaving
against a zero-filled buffer, which would cost 4 MB and a load per sample.

One place where the obvious version of this is **not safe**, because
`stream::setBufferSize()` frees a buffer a running consumer may still be
reading, and `TempStop` stops a block but not its reader:
`BroadcastFM::setSamplerate()` grows `rdsOut`, from the static preview, before
it reconfigures `rdsResamp`. It never shrinks it, and it may do even that only
under a stated precondition that the caller has stopped the RDS consumer. It
has no caller: WFM's IF rate is the constant 250 kHz.

What is left is genuinely full-rate, and shrinks only when a producer's block
size becomes knowable (§7 items 1-2). At the ~16 000-sample blocks §1.3
observes in practice rather than the 1 000 000 the buffers are built for, the
retained ~84 MB would fall to about 1.4 MB.

### 2.5 The same patterns elsewhere

Sized for their own rate ("at full capacity" is the size each would have if
built from `STREAM_BUFFER_SIZE`):

    site                            what it runs at            at full    sized
    -----------------------------   ------------------------   -------    -------
    RxVFOKernel post-resample       the VFO output rate,
      filter delay line               not the input rate
      - demod VFO, 2.4 M -> 250 k                               8.0 MB    0.83 MB
      - ctcss.ddc, 50 k -> 500                                  8.0 MB    0.08 MB
    CTCSSSquelch ddcOut + fmOut     fmOut at 500 Hz; ddcOut
                                      is full-rate workspace   12.0 MB    8.04 MB
    VOR amvOut + fmvOut + out       out at 1 kHz; amvOut and
                                      fmvOut are full-rate     24.0 MB   16.32 MB
    VOR magOut                      an envelope buffer          4.0 MB    4.00 MB
    POCSAG out + soft buffer        MM minimum phase step       6.0 MB    0.71 MB
    diagnostic FrameTaps            2 x a fixed `keep`               —    ≤ 32 KB
    IQ front-end FFT                frames are windowed
                                      straight into FFTW input       —    no stream

VOR's envelope is a plain 4 MB buffer; a `dsp::demod::AM<float>` in its place
would bring an output stream, a carrier-AGC buffer and an LPF delay line, about
20 MB, none of which the receiver uses. VOR and POCSAG are per-module and only
pay when that decoder is loaded.

The `RxVFOKernel` filter is the most widely shared case: it runs *after* the
resampler, so it sees the output rate, and every VFO in the application owns
one. Its caller-provided output buffer is different: the fused kernel first
writes the full-rate frequency-translator result there, so that buffer cannot
shrink to the final resampler count.

Not applied, and why:

* **RadioModule's AF chain.** CW interpolates 3 kHz -> 48 kHz while WFM
  decimates 250 kHz -> 48 kHz, and the demodulator changes at runtime under a
  chain that stays alive. Sizing it needs §1.3's analysis, not a ratio.
* **Every `dsp::stream` whose size can change at runtime**, including the VFO's
  own output. The consumer is a different block that the owner cannot stop, so
  `setBufferSize()` would free a buffer a running reader may hold. Streams whose
  bound is fixed for the life of the object *can* be sized at `init()`, which is
  what the VOR, POCSAG and RDS output streams above do; the rest is §7.1.
* **The Meteor demodulator**, where the arithmetic does not pay. It decimates
  only 150 kHz -> 72 kHz symbols, ~2:1, and `process()` uses `out.writeBuf` as
  its full-rate scratch for the RRC, AGC and Costas stages before the clock
  recovery narrows it. Shrinking `out` would mean adding a full-rate scratch
  buffer of its own: 16 MB becomes 8 + 7.7, a 0.3 MB saving for a restructured
  `process()`.

### 2.6 Rate-derived buffer sizes — review index

**Every allocation below is derived from the producer that writes it, not from
`STREAM_BUFFER_SIZE`.** Each is marked in the source with `[RATE-SIZED]`:

    grep -rn '\[RATE-SIZED\]' core/src decoder_modules

None of these buffers is bound-checked at the point of writing (§1, §7 item 1).
Their capacity is an argument rather than a constant, so a wrong bound is a
silent heap overflow, not dead space. The bounds are structural: each is
calculated from the same plan or loop limits that govern the producer's data
path.

There are two distinct contracts. `maxOutputBlock()` is the largest count a
producer returns or publishes. `maxOutputBuffer()` is the largest span it may
write in the caller's destination while producing that count; fused and
multi-stage kernels can require much more workspace than their final output.

    site                                      bound derived from
    ---------------------------------------   -------------------------------
    PowerDecimator::reconfigure() stageMax    the decimation plan's own
                                                per-stage ratios
    PowerDecimator::maxOutputBlock()           the final cumulative decimation
    PowerDecimator::maxOutputBuffer()          the largest intermediate stage
                                                output
    RationalResampler::reconfigure()          predecRatio, the power of two
      resampMax                                 the decimator above applies
    RationalResampler::maxOutputBlock()       predecRatio plus the selected
                                                resampInterp / resampDecim
    RationalResampler::maxOutputBuffer()      the larger of the final output
                                                and power-decimator workspace
    RxVFOKernel::filterMaxBlock()             RationalResampler's actual
                                                configured output bound
    RxVFOKernel::maxOutputBuffer()            full-rate xlator output or the
                                                resampler workspace, whichever
                                                is larger
    BroadcastFM RDS stream storage            rdsResamp.maxOutputBuffer()
    BroadcastFM RDS published count          rdsResamp.maxOutputBlock()
    CTCSSSquelch ddcOut                       RxVFOKernel::maxOutputBuffer()
    CTCSSSquelch fmOut                        RxVFOKernel::maxOutputBlock()
    vor::Receiver amvOut/fmvOut               RxVFOKernel::maxOutputBuffer()
    vor::Receiver output stream              RxVFOKernel::maxOutputBlock()
    MM::maxOutputBlock()                      the loop's minimum legal phase
                                                advance after frequency/error
                                                clamping
    POCSAGDSP output stream, soft buffer      MM::maxOutputBlock()
    PowerDecimator::maxOutputBlockForRatio()  the same plan arithmetic,
      / maxOutputBufferForRatio()               without an instance
    RationalResampler::                       calculateRatePlan(), the very
      maxOutputBlockForRates()                  function reconfigure() commits
      / maxOutputBufferForRates()
    RxVFOKernel::                             the resampler previews above,
      maxOutputBlockForInSamplerate()           for a pending input rate
      / maxOutputBufferForInSamplerate()
    SubWaterfall::resampledV                  maxOutputBufferForRates()
      (core/src/gui/brown/small_waterfall.cpp)

**Why the bounds must be structural.** A flat rate ratio plus slack for
retained phase is wrong for all four of the cross-rate destinations, in the
dangerous direction. With `C` = 1 000 000, the default input bound:

    site                ratio estimate     actual write        why
    ------------------  -----------------  ------------------  ---------------------------
    CTCSS ddcOut        ceil(C/100) + 64   C                   the fused xlator writes the
                                                                full-rate block first
    VOR amvOut, fmvOut  ceil(C/25) + 64    C                   the same
    BroadcastFM rdsOut  ceil(C/50) + 64    ceil(C/8)           plan_32's first stage writes
                                                                its intermediate there
    POCSAG output       ceil(C/10) + 64    ceil(C/8.5)         MM may advance 8.5 samples
                                                                per symbol, not 10

A flat ratio also ignores the resampler's own ratio selection. Near-unity
approximation can select passthrough, which a 5001 -> 5000 estimate undersizes
by 135 samples, and a ratio the power decimator absorbs entirely is undersized
by 60 in a 2001 -> 500 estimate. Both are regression cases in
`rational_resampler_tests`. Each of these needs a block far longer than the
tens of milliseconds the audited sources deliver (§1.3; `sr/200` producers send
5 ms) — 0.2 s of 50 kHz NFM audio for CTCSS, 0.64 s of 250 kHz IF for RDS,
1.6 s of VOR input, and ~35 s for POCSAG with the loop pinned at its frequency
limit — but §4's peer-controlled producers can deliver blocks that long.

Six properties worth checking explicitly when this is reviewed:

1. **Final output and workspace are deliberately separate.** A 250 kHz ->
   5 kHz RDS resampler returns at most 20 001 samples but writes a 125 000-sample
   first-stage intermediate. `RxVFOKernel` needs still more because its xlator
   writes the full input block before the resampler runs.
2. **Preview APIs and live configuration use the same pure rate plan.** This
   includes upward approximation and near-unity passthrough, and lets owners
   grow a destination before changing a live producer. Reconfiguration paths
   retain a larger old allocation rather than shrink before a throwing setter.
3. **`BroadcastFM` publishes the selected RDS final-output bound.**
   `getRDSMaxBlock()` hands it to `RDSDemod::init()`, which sizes its output
   stream and four buffers from it. It is the only one of these bounds that
   crosses a module boundary, and nothing re-checks it on the other side.
4. **None of the bounds clamps down when the ratio exceeds 1.** A VFO or resampler
   configured to interpolate produces *more* samples than it consumes, and
   clamping to `maxBlock` there would under-size rather than merely waste. The
   code deliberately returns the larger figure; §1.2 notes that nothing rejects
   such a configuration in the first place.
5. **A capacity change is not a signal discontinuity.**
   `HistoryBuffer::setMaxBlock()` and `FIR::setMaxBlock()` keep the retained
   samples, so a source-rate change does not glitch the VFO filter.
   `RxVFOKernel` resets its post-resampler FIR only where the samples really
   are stale: after an output-rate change (new sample spacing) and when the
   filter comes out of bypass.
6. **A failed reconfiguration emits nothing.** Behind a rate-sized
   destination, passthrough would overrun by up to the whole input block, so a
   failed `RationalResampler::reconfigure()` leaves `Mode::DROP`, which emits
   nothing until a later configuration succeeds (§5.2).

---

## 3. Producer block sizes

Maximum elements each producer writes into a `writeBuf`. These are **audited
upper bounds derived by reading the code**, not measured values, and for
hardware sources they depend on the pinned dependency revision and the device
family — both of which should be recorded when a bound is relied upon.

**This audit is incomplete.** See §4 for producers whose block a peer or user
controls, and §1 for why a complete source audit still would not establish the
invariant.

### 3.1 Fixed size — independent of sample rate

    source      max block   derivation
    ---------   ---------   ---------------------------------------------------
    hackrf        131 072   TRANSFER_BUFFER_SIZE 262144 B, valid_length/2
                            (hackrf_source/src/main.cpp)
    airspy         65 536   buffer_size 262144, FLOAT32_IQ -> /2 then /2;
                            packing off (libairspy airspy.c)
    hydrasdr       65 536   DEFAULT_BUFFER_SIZE 262144, same math
    rtl_sdr         7 936   roundf(3.2e6/102400) = 31, x512 = 15872 BYTES;
                            the callback does len/2
    kcsdr           4 096   0x4000/4. Note: the divisor is the literal 4, not
                            sizeof(complex_t) — 0x4000/8 would be 2048 and is
                            wrong.
    airspyhf        4 096   SAMPLES_TO_TRANSFER = 1024*4
    perseus         2 048   clamp(sr/200/1024, 1, 2) * 1024 — hard-capped
    qmxserver         512   EXT_BLOCKLEN (qmxserver_client.h), and the packet
                            length is checked against it before the write
    kiwisdr           512   onSamples(decoded, 512), fixed
                            (core/src/utils/proto/kiwisdr.cpp)
    qmx               512   qmx::kStreamBlockSize (QmxDevice.h)

`hackrf` is the largest fixed-size producer audited. These copy whatever the
library hands them, so they are not clamped by `sourceBlockSize()`.

### 3.2 Sample-rate dependent — at each device's top rate

All of these derive their block with `dsp::sourceBlockSize(sr, stream,
divisor)`, which clamps it to `[1, stream.capacity()]`. The "max block" column
is the unclamped figure at the device's top rate; a figure above the capacity
is clamped to it.

    source     formula          max rate       max block   rate source
    --------   --------------   ------------   ---------   --------------------
    usrp       sr/200           250 MS/s       1 250 000   UHD query
    sddc       sr/100            64 MS/s         640 000   list caps
    limesdr    sr/200          61.44 MS/s        307 200   LMS API query
    bladerf    sr/200, ->1024  see note          see note  srRange->max
    fobossdr   sr/400            50 MS/s         125 000   fobos_rx_get_
                                                           samplerates
    sdrplay    sr/200            10 MS/s          50 000   list caps
    rfspace    sr/200 + one    1.2288 MS/s         ~6 200   default sr; list is
               packet                                      device-reported
    rtl_tcp    sr/200           3.2 MS/s          16 000   list caps
    hermes     sr/200 + <=62    384 kS/s           1 982   list (hermes.cpp)
    audio      sr/200           192 kHz              960   RtAudio
    soapy      sr/200          device-dep.     clamped     backend-defined

Note on **bladerf**: the rate range is read from the device at runtime
(`bladerf_get_sample_rate_range`), and some bladeRF2 configurations report above
61.44 MS/s. Do not treat any fixed figure here as a hard bound; it must be taken
from the pinned library and device family in use.

Note on **rfspace** and **hermes**: they accumulate packets into `writeBuf` and
publish before the next packet would not fit, so they stay within capacity.

### 3.3 Self-bounding

These track the constant or the destination's capacity automatically and need
no attention when it changes. They are the reference pattern for §6.

    plutosdr        std::min<int>(sr/20, STREAM_BUFFER_SIZE)
    spectran        rejects pkt.num > STREAM_BUFFER_SIZE
    harogic         maxBufCount = STREAM_BUFFER_SIZE / bufferSize
    rfnm            maxBufCount = STREAM_BUFFER_SIZE / sampCount
    file_source     clamp(sr/200, 1, STREAM_BUFFER_SIZE)
    network         UDP read and TCP/SCTP block bounded by the
                    stream's capacity()
    sdriak server   DECOMP_INPUT_BUFFER_SIZE from the wire limit
                    SERVER_PROTOCOL_MAX_BLOCK, output split to capacity(),
                    payload size validated on receive

PlutoSDR, harogic, rfnm and Spectran (USB) clamp against
`STREAM_BUFFER_SIZE`, not `capacity()`, which is correct only while their
stream is not resized; none is.

### 3.4 Not audited

    dragonlabs   callback `count` from libdlcr, which is not vendored in this
                 tree; device fixed at 12.5 MS/s. Must be closed before the
                 constant moves.

---

## 4. Externally controlled producers

Producers whose block size a peer or the user decides. Each is bounded:

* **`network_source`.** UDP reads into a buffer bounded by the stream's
  `capacity()`. TCP/SCTP uses `blockSize = samplerate / 200` with a
  user-entered rate clamped only from below, so the block is clamped to
  `[1, capacity()]`.
* **SpyServer.** `BodySize` is compared against
  `SPYSERVER_MAX_MESSAGE_BODY_SIZE` (`spyserver_protocol.h`) before the read,
  and a larger body disconnects. Even within the 1 MiB cap, `UINT8_IQ` yields
  `BodySize / 2` = 524 288 complex samples, twice a 262 144 stream, so IQ is
  published in pieces of at most the stream's `capacity()`. An empty body is
  legal and is not treated as a disconnect.
* **SDRIAK server client.** Payload sizes are validated on receive and the
  decompressor splits its output to `capacity()`
  (`buffer-size-requirements-current.md` §4.1.1).
* **Spectran HTTP.** The metadata line is capped explicitly, and the sample
  data after it is read in pieces of at most one stream buffer, each published
  separately.
* **RFspace TCP control header.** A peer length below 2 disconnects, since
  `size - 2` would be negative and become a near-unbounded `size_t` read into
  an 8 KB buffer. RFspace is loaded on Android.
* **USRP, Soapy and LimeSDR.** The library call receives
  `dsp::sourceBlockSize()` as its capacity, and LimeSDR publishes only what it
  received, after checking for an error.
* **QMX Server.** The live data path is the ENet receive, which uses a fixed
  `EXT_BLOCKLEN` of 512 and checks `event.packet->dataLength` against it before
  writing. The file also carries a textual clone of the SpyServer handler,
  including an unvalidated `readSize(BodySize, readBuf)`, inside an `#if 0`
  block that is never compiled. Do not "fix" the dead code.

What is not bounded is a fixed-size library callback larger than the stream
(§3.1), which matters only below 131 072.

---

## 5. The 32-bit / Android case

`android/app/build.gradle` builds `armeabi-v7a` alongside `arm64-v8a`. On
`armeabi-v7a`, an allocation can fail during `MainWindow::init()` →
`ModuleManager::createInstance("Radio", …)` → `RadioModule::selectDemodByID()` →
`demod::WFM::init()`.

`dsp::buffer::alloc()` reports the requested size and throws on failure, rather
than returning `volk_malloc`'s NULL into the `buffer::clear()` every caller
follows it with. The FFTW allocations go through `dsp::buffer::FFTWBuffer`
(§6.1), which checks too. Note that `volk_malloc()` also returns NULL for a
**zero-sized** request, so `alloc(0)` throws with a misleading out-of-memory
message; `FFTWBuffer::allocate()` rejects a non-positive count with its own
message instead. `taps::estimateTapCount()` returns
`3.8 * samplerate / transWidth`, which is 0 whenever the transition width
exceeds 3.8x the sample rate — not reachable from any call site, but the same
shape as the non-positive rate §1.4 rejects.

### 5.1 OOM is fatal, by design

An allocation failure must not be swallowed and execution must not continue:

> A module constructor registers global state before it can fail, and when a
> constructor throws, the most-derived object's destructor does not run — only
> fully constructed member and base subobjects are destroyed.

`RadioModule` is the worked example. Before it reaches the demodulator it has
called `sigpath::vfoManager.createVFO()`, bound a waterfall handler carrying
`this`, and called `sigpath::sinkManager.registerStream(name, &stream)`. The
matching teardown lives in `~RadioModule`, which never runs. Continuing would
leave `sinkManager` holding a pointer into freed memory and the waterfall
holding a dangling `ctx` — a later use-after-free in place of a deterministic
crash at startup.

So the guarantee is only: *diagnosable and immediate*, not *recoverable*.

### 5.2 What recoverability would require first

**Allocations are released on the throw path.** Block destructors bail on
`if (!_block_init) { return; }` (e.g. `core/src/dsp/demod/broadcast_fm.h`), and
`_block_init` is only set by the `base_type::init(in)` call at the *end* of
`init()`. That does not matter for the allocations, because every one has an
owner (§6.1) and **member destructors run regardless of that guard**, even
when the owning constructor itself throws. An `init()` that throws part-way
releases what it had taken, at the moment memory is scarcest. There is no raw
owning pointer in the radio graph.

**Restarts are coherent.** `start()` and `tempStart()` join whatever workers a
throwing `doStart()` created before it failed (a block may start two), clear
`running` and `tempStopped`, and leave the block restartable. `stop()` during a
temporary stop cancels the pending restart instead of re-running `doStop()` on
streams that may since have been rebound. `block_restart_tests` covers all
three.

Still open, and each has to be closed before an OOM could be *caught* rather
than merely diagnosed:

* **A failed reconfiguration restarts the block anyway.** `block::TempStop`
  always calls `tempStart()` in its destructor, including while the exception
  that aborted the reconfiguration is still unwinding, so the worker resumes
  against whatever state the throw left. Every setter therefore has to be
  transactional (§7 item 6). `FMIF::setBins()` is the worked example:
  `initBuffers()` releases everything and rethrows, and `process()` checks
  `configured` and passes through, so the restarted worker does not fault.
  Making the guard *not* restart is not the fix — skipping `tempStart()` leaves
  `tempStopDepth` elevated, and the block then never restarts at all.
* **Composites mutate sub-objects one at a time.** Most handle it:
  `RationalResampler::reconfigure()` drops to `Mode::DROP` up front and
  re-commits the mode at each successful exit; `RxVFOKernel` marks its rates
  invalid across the resampler rebuild, so the next setter retries even with
  the same values; `PowerDecimator::reconfigure()` and `MM::setInterpParams()`
  build replacements into locals before committing; `CTCSSSquelch::
  setSamplerate()` and `BroadcastFM::setSamplerate()` grow their rate-sized
  destinations from the static previews *before* touching the producer and
  never shrink them. `BroadcastFM::setSamplerate()` still walks its other
  sub-objects in sequence with no rollback.

None of this changes §5.1: OOM is fatal by design, because a module
constructor registers global state before it can fail.

### 5.3 Lowering the constant — preliminary figures only

**These numbers do not constitute a safety case.** §3 is incomplete and §3.1's
fixed callbacks are not clamped. §7 is the gate.

Among sources both built **and loaded** by Android's hard-coded module list, the
largest audited fixed-size producer is hackrf at 131 072. SDDC is
`OPT_BUILD_SDDC_SOURCE=OFF` by default, USRP needs UHD, and
BladeRF/LimeSDR/FobosSDR are not in the Android build. Network Source and
Spectran HTTP are compiled for Android but absent from the hard-coded list in
`core/src/core.cpp`, so normal Android users cannot load them. Dragonlabs is
also absent from that list. SpyServer is loaded, and splits its output to
capacity (§4), as does the SDRIAK server client. The interpolation findings in
§1 also mean that "nothing exceeds 131 072" cannot be established from the
source table alone. See §7.1 for the order in which this should be approached.

    candidate   vs. audited hackrf   constructed   peak     retained
    ---------   ------------------   -----------   ------   --------
    1 000 000   7.6x                       34 MB    86 MB      84 MB
      262 144   2.0x  (32-bit Android)    8.9 MB    23 MB      22 MB
      131 072   1.0x  (no margin)         4.5 MB    11 MB      11 MB
       65 536   0.5x  (must fragment)     2.2 MB   5.7 MB     5.5 MB

Peak is a separate column because initialisation overshoots its end state
(§2.2), by RDSDemod's `out` alone, at every capacity. Nothing in these columns
is fixed: every one scales linearly with the capacity. The retained figure is
`84.4 x capacity`, and the 84.4 bytes per element break down as:

    full-rate FIR and delay lines (pilot, L+R, L-R, two audio)     28.0
    full-rate scratch (five BroadcastFM buffers)                    28.0
    streams (out 16, rdsOut 2.0, RDS out 0.04)                      18.0
    rdsResamp decimator stages (8 + 1 + 0.5) and polyphase line      9.8
    the rest of the RDS branch (RDSDemod's fir, MM, scratch and
      soft buffer)                                                   0.5

The 2-byte `rdsOut` is the decimator's workspace, not its 5 kHz output. The
`constructed` column is a stream count: a `dsp::stream` is built at
`STREAM_BUFFER_SIZE` by its constructor and only narrowed by `setBufferSize()`
during `init()`, so rate-aware sizing shows up in the retained column. The
figures are WFM-only; see §2.3.

The polyphase taps and phase banks (§1.4, §2.1) are **not** in these columns and
do not scale with the constant — they scale with the resampling ratio. At the
current ratios they total 0.875 MB across the radio graph.

Lowering the constant on **all** platforms does not need the rate-derived
sources changed: they follow the stream's capacity (§3.2). Fixed-size library
callbacks, HackRF's first, need splitting below 131 072.

---

## 6. Rule for new producers

A producer must never assume the constant's value. Either:

* derive the block size with `dsp::sourceBlockSize(sr, stream)`, which clamps
  `sr/200` to the stream's `capacity()`, as every rate-derived source does; or
* validate a library- or peer-supplied count before copying, as `spectran` and
  `qmxserver` do. Check it is not *too small* as well: `length - header` on a
  short value underflows (§4, RFspace).

Clamp against *the destination stream's actual capacity*, not the global
default: `sample_stream_compressor.h` and `sdriak_server_client.cpp` call
`setBufferSize()`, so streams of non-default size exist.

`sr/200` written bare overflows as soon as the rate is large: it is safe only
while the rate is known and small, and that assumption dies the moment the rate
list, the hardware, or a user-editable field changes.

Any block whose output count can exceed its input count (§1) must validate
output capacity before writing, not after.

### 6.1 Ownership: use an owner, not a raw pointer

Every allocation in the DSP graph has an owner type. New code uses one.

    owner                                allocator          for
    ----------------------------------   ----------------   ---------------------
    dsp::buffer::Buffer<T>               volk_malloc        SIMD scratch and
                                                            working buffers
    dsp::buffer::HistoryBuffer<T>        volk_malloc        a delay line: the
                                                            last H samples plus
                                                            one block <= maxBlock,
                                                            exactly H + maxBlock;
                                                            setMaxBlock() keeps
                                                            the history
    dsp::tap<T>                          volk_malloc        a generated tap set
    dsp::multirate::PolyphaseBank<T>     volk_malloc        a phase bank
    dsp::buffer::FFTWBuffer<T>           fftwf_malloc       FFT working buffers
    dsp::buffer::FFTWPlan                fftwf_plan_*       an FFT plan

Four properties they share:

* **They free on the throw path.** Member destructors run even when the owning
  `init()` or constructor throws, which is what §5.2 turns on.
* **`allocate()` acquires before it releases.** A failed reallocation leaves the
  owner on its existing storage rather than empty, so a caller that catches is
  not left with a dangling derived pointer. Recompute any `bufStart`-style
  pointer after it returns.
* **They are move-only, with the move operations declared explicitly.**
  Declaring copy operations suppresses the implicit moves. `tap<T>` and
  `PolyphaseBank<T>` are the owners themselves, with no plain-struct
  `operator=` for a `std::move` to bind to by mistake. The FIR family (`FIR`,
  `DecimatingFIR`, `FIRBlock`) owns the taps it is given as an rvalue — every
  call site in the tree passes a generator result or `std::move` — and frees
  the previous set only after the replacement is published. An lvalue is
  borrowed instead, for a set shared with something else; the FIR tracks which
  case it is in. `PolyphaseResampler` copies the taps into its phase bank and
  keeps no reference, so its callers hold none either.
* **The allocator is part of the type.** `FFTWBuffer` exists separately from
  `Buffer` because an FFT plan records the *alignment* of the arrays it was
  built against: reallocating a buffer invalidates the plan, so
  `FFTWPlan::planDFT1D()` makes re-planning one step rather than something to
  remember. `FFTWPlan` also holds the process-wide planner lock across every
  FFTW call, since only `fftwf_execute()` is thread-safe and this tree plans
  from the UI, a source thread and a DSP worker. That lock is a single
  instance exported from core (`core/src/dsp/buffer/fftw_mutex.cpp`), so
  planning in module DLLs such as `dab_decoder` or `logmmse` is serialized
  with planning in core. It is deliberately never destroyed: plans owned by
  static objects such as `sigpath::iqFrontEnd` are destroyed after any
  function-local static first touched later, and libc++ and bionic abort on
  locking a dead mutex.

Two mechanical consequences at call sites. `Buffer`/`FFTWBuffer` convert
implicitly to `T*`, but a **C-style cast cannot see through a user conversion**,
so `(lv_32fc_t*)buf` must be written `(lv_32fc_t*)buf.get()` — a compile error,
not a silent bug. And template argument deduction does not consider user
conversions either, so `buffer::clear(buf, n)` must name the type:
`buffer::clear<T>(buf, n)`.

Ownership must also be declared **before** whatever reads it, since members are
destroyed in reverse order. With the FIRs owning their taps this does not
apply to tap sets; it applies to any borrowed set a caller hands a FIR by
lvalue.

---

## 7. What must land before the constant changes

1. **A bounded-write path in `dsp::stream`.** The stream stores its capacity,
   updates it in `setBufferSize()`, exposes it as `capacity()`, and `swap()`
   aborts with a logged message on an oversized count. That check is secondary
   corruption detection, not the primary guard: it runs after the write.
   Missing: a bounded-write or reservation API, and moving the remaining
   producers from `STREAM_BUFFER_SIZE` to `capacity()` (§3.3).
2. **Phase-aware pre-write handling in expanding blocks** —
   `PolyphaseResampler`, `RationalResampler`, `RRCInterpolator`, `RxVFO` and any
   other block whose output count can exceed its input count. The prediction
   half exists: the kernels take a `maxBlock` bound on their input and publish
   `maxOutputBlock()`/`maxOutputBuffer()` and static previews (§1.1, §2.6).
   Missing: the destination's real capacity to compare against (item 1), and
   fragmenting the output when it is smaller.
3. **Test the RRC interpolator.** Its output counts and reconfiguration are
   uncovered (§1.5).
4. **Split fixed-size library callbacks** larger than the stream (§3.1), and
   **audit §3.4** (dragonlabs).
5. **Tally whole-process construction**, especially RadioModule's own members
   (§2.3), not just the demodulator. §2.1–2.3 follow the member lists line by
   line, so the weakest numbers are the ones not there at all: the radio's VFO,
   the IQ front end, waterfall history and the other modules.
6. **Make a failed reconfiguration leave a coherent block** (§5.2): every
   reconfiguration has to be transactional — acquire everything into locals,
   commit with no-throw moves. That is how `FIR::setTaps()`,
   `PolyphaseResampler::setRatio()`, `RationalResampler::reconfigure()`
   (`Mode::DROP` until it commits), `CTCSSSquelch::setSamplerate()`,
   `RxVFOKernel` (the tap sets, and its rate setters, which size the filter
   from a preview before touching the resampler and retry after a failed
   rebuild), `Splitter::bindStream()`, `IQFrontEnd::addVFO()` and
   `IQFrontEnd::updateFFTPath()` work. Open: `BroadcastFM::setSamplerate()`
   grows `rdsOut` first but then walks its other sub-objects with no rollback.
7. **Reproduce on the affected device**, recording ABI, the failing allocation
   size, native allocation profile, RSS/PSS and `/proc/<pid>/maps`. The failure
   depends on existing mappings, fragmentation and device limits, and none of
   that has been measured. A WFM peak of ~86 MB may well fit where the original
   failure did not — **that would not close this item.** It moves the failure
   threshold rather than establishing one, and a radio session holds ~292 MB
   without counting the VFO, the front end or the waterfall (§2.3).

### 7.1 Staging

The chosen direction is to preserve the Android source-module set and make
large producers and expanding DSP blocks capacity-aware. Module removal is not
a substitute for the missing capacity contract.

    stage   action                                         depends on
    -----   --------------------------------------------   ----------
    next    make resamplers phase-aware and fragment their
            output; test RRC                               items 1-3
    next    fragment fixed callbacks such as HackRF
            where required                                 item 4
    then    verify the 32-bit constant of 262 144 on a
            device                                         item 7
    last    go further, to 131 072 and then 65 536         all

262 144 was chosen because HackRF's 131 072, the largest block any source
loaded on Android writes, then has 2x margin and needs no fragmentation. The
reasoning is in `buffer-size-requirements-current.md` §8.

At 131 072, HackRF exactly fills a stream and has no margin. At 65 536 it must
be split in two, while Airspy and HydraSDR exactly fill the stream. Values below
65 536 require progressively more source fragmentation. Section 1.3 shows that
the live radio interpolation path does not by itself justify 262 144, but it
must still be guarded before any smaller value becomes a correctness claim.
