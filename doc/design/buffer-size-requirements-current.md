# Buffer sizes — current state

Read from the tree of 2026-11-15. This is the short version of
`buffer-size-requirements.md`: what is true and what is open. Figures are
counted from the code, not measured.

---

## 1. The problem in one paragraph

Every `dsp::stream<T>` allocates two buffers of `STREAM_BUFFER_SIZE` elements
in its constructor (`core/src/dsp/stream.h`): **262 144 on 32-bit Android,
1 000 000 everywhere else**. A producer writes into `writeBuf` and then calls
`swap(n)`. The stream records its capacity (`capacity()`), and `swap()`
aborts with a logged message if `n` exceeds it. That check runs *after* the
write, so it turns silent heap corruption into an immediate, named failure;
it does not prevent the overflow. **A producer must never write more than the
buffer holds.** So the constant is not a tuning knob: it is a contract between
every producer and every buffer it writes, and it is also most of the
application's native memory.

§8 records why 262 144 is safe on 32-bit Android and what blocks going lower,
or lowering it elsewhere.

## 2. Status of the Android OOM

An allocation failure in `demod::WFM::init()` at startup on `armeabi-v7a` is
the case this work is driven by.

* **Diagnosable:** `buffer::alloc()` and the FFTW owners throw with the
  requested size instead of returning NULL into `memset`. An OOM is fatal by
  design. A module constructor has registered global state (VFO, waterfall
  handler, sink stream) before it can fail, and a throwing constructor never
  runs its destructor, so continuing would leave dangling pointers
  (`core/src/module.cpp`).
* **Footprint.** The startup peak of a radio session is ~294 MB at the desktop
  constant (~663 MB in upstream SDR++), and should be ~77 MB at 32-bit
  Android's 262 144 (§8).
* **Not confirmed on a device.** What usually fails on 32-bit is finding a
  contiguous 8–16 MB block in a fragmented address space, and that has never
  been measured. To close it, run on the device in WFM and record ABI,
  RSS/PSS, `/proc/<pid>/maps` and the size of any failing allocation.

## 3. What memory goes where

All figures assume `STREAM_BUFFER_SIZE` = 1 000 000, the desktop and 64-bit
value. §3.4 gives the 32-bit Android value.

### 3.1 Unit costs

    one dsp::stream (two buffers)    complex/stereo 16 MB, float 8 MB, uint8 2 MB
    one scratch Buffer<T>            half a stream: complex 8 MB, float 4 MB
    one delay line (HistoryBuffer)   exactly maxBlock + history elements
    one FrameTap                     2 x keep elements

Most DSP classes are **kernels**: a fused computation with no stream and no
thread (FIR, Delay, resamplers, AGC, PLL, MM, `RxVFOKernel`...). Only the
**blocks** (demod composites, `RxVFO`, `FIRBlock`, `RationalResamplerBlock`,
squelches...) own an output stream. Diagnostic taps (`dsp::buffer::FrameTap`)
are neither: a `2 × keep` history the producer fills inline, with no stream
and no thread. To estimate cost, count blocks for streams and kernels for
delay lines.

### 3.2 The WFM demodulator

    constructed                                   34 MB   (3 streams)
    peak during init()                            86 MB
    retained afterwards                           84 MB

The retained 84 MB:

    full-rate delay lines (pilot FIR, L+R and L-R delays, 2 audio FIRs)   28.0
    full-rate scratch (5 BroadcastFM buffers)                             28.0
    BroadcastFM output stream                                             16.0
    RDS resampler: decimator stages 9.5, polyphase line 0.25               9.8
    RDS branch: rdsOut 2.0 plus RDSDemod's buffers and soft tap 0.55       2.6

RDSDemod keeps its soft symbols in a private RDS-rate buffer and feeds a
32 KB `FrameTap` for the symbol display; there is no diagnostic stream.

The 2 MB overshoot during `init()` is RDSDemod's `out`, which is constructed
at full size and shrunk only at the end of its `init()`, after BroadcastFM's
scratch is allocated. `rdsOut` is also built full-size but is narrowed before
the scratch. Fixing that needs streams that can be built without buffers
(§5.1 step 3).

### 3.3 The rest of a radio session

RadioModule builds its own chain before the demodulator exists:

    10 streams (nb, fmnr, powerSquelch, dummy audio, ctcss, resamp,
                hpf, deemp, sink volumeInput, volumeAjust)            160 MB
    init allocations (fmnr 8, ctcss 17.5, AF resampler 14, hpf 8)      47.5 MB
    RadioModule retained                                              207.5 MB

    RadioModule + WFM peak                                            ~294 MB
    RadioModule + WFM retained                                        ~292 MB

**Not counted:** the radio's VFO, the IQ front end, waterfall history, the
source module, other modules and allocator overhead. The real process figure
is higher.

### 3.4 What a smaller constant would give (WFM only)

Retained ≈ `84.4 bytes × capacity`. Nothing in it is fixed apart from a few
tens of KB of taps and the RDS soft tap, so all three columns scale linearly.

    capacity    vs. HackRF's 131 072 block   constructed   peak    retained
    ---------   --------------------------   -----------   -----   --------
    1 000 000   7.6x                               34 MB    86 MB     84 MB
      262 144   2.0x (32-bit Android)             8.9 MB    23 MB     22 MB
      131 072   1.0x (no margin)                  4.5 MB    11 MB     11 MB
       65 536   0.5x (HackRF must fragment)       2.2 MB   5.7 MB    5.5 MB

**This table is not a safety case.** See §4 and §8.

## 4. Why the constant cannot simply be lowered

### 4.1 Sources: every block bounded by the stream

Every source's block is bounded by its stream's capacity. Most that compute
the block size from the sample rate use
`dsp::sourceBlockSize(samplerate, stream, divisor = 200)` in
`core/src/dsp/stream.h`, which clamps the result to `[1, stream.capacity()]`;
the exceptions below clamp against the constant, or use a fixed or
rate-list-derived block, instead.
Those that accumulate packets into `writeBuf` also publish early when the next
packet would not fit.

* **USRP, Soapy and LimeSDR.** UHD, SoapySDR and LMS treat the count as the
  caller's buffer capacity, so they receive the clamped figure: unclamped,
  `sr/200` is 1 250 000 at USRP's 250 MS/s and whatever a Soapy backend
  reports. LimeSDR publishes only what `LMS_RecvStream()` returned, after
  checking it for an error.
* **The rest** use the same helper: bladeRF (rounded down to 1024), SDDC
  (`sr/100`), FobosSDR (`sr/400`, bounded by both of its destinations),
  SDRplay, the audio source, rtl_tcp, Hermes, RFspace and the file source. The
  audio source's RtAudio callback also splits a period larger than the
  stream, because RtAudio may pick a different period than the one requested.
  Hermes and RFspace accumulate frames, so they publish early before a frame
  would overrun.
* **Clamped against the constant:** Perseus (hard-capped at 2048), KCSDR
  (fixed 4096), PlutoSDR, harogic and rfnm. That equals their stream's
  capacity, because none of those streams is resized.
* **Dragonlabs** is not audited (its library is not in the tree).

The `swap()` check is a backstop for anything missed: an oversized count
aborts with a message, but only after the write.

### 4.1.1 Network input: sample buffers bounded

Every network source bounds the samples it writes by what it reads or by the
stream's `capacity()`. That is a claim about sample buffers. The other
peer-supplied fields are checked where noted below, but not audited
exhaustively: treat a new length or text field from a peer as unbounded until
shown otherwise.

* **RFspace** (loaded on Android). The TCP control channel reads `size - 2`
  bytes into an 8 KB buffer with a 13-bit length from the peer, so lengths
  below 2, which would make that negative and turn into a near-unbounded
  `size_t` read, disconnect. A header-only message is not a disconnect, a
  zero-length data item is read as the protocol's 8194 bytes, and the device
  ID is read only from a payload long enough to hold it. On UDP, a data item
  can legally be 8194 bytes, encoded as length 0 (NetSDR Interface
  Specification), so the UDP path reads into a buffer as large as any
  datagram, drops anything shorter than a header or longer than 8194 bytes,
  reads length 0 as 8194, and publishes what has accumulated before a packet
  would overrun the stream.
* **Spectran HTTP** (built, not loaded on Android) rejects a chunk shorter
  than its metadata and reads the data in pieces of at most one stream
  buffer, publishing each. Its non-sample input is bounded too:
  - the HTTP response header goes through `http::Client::recvHeader()`,
    capped at 64 KiB in total and 8 KiB per line, like
    `recvResponseHeader()`; an oversized header is an error, not an
    exception;
  - the metadata line is capped at 64 KiB, since narrowing the peer's
    `size_t` chunk length to `recvline()`'s `int` can produce 0, which means
    "no limit";
  - the JSON fields are parsed with `std::from_chars`, so a missing or
    malformed field rejects the chunk instead of throwing out of the worker
    thread, which would terminate the process. Frequencies outside
    [0, 1 THz], an inverted range and a samplerate outside (0, 1 GS/s] are
    rejected, so the span and the integer midpoint cannot overflow;
  - the worker has a catch-all as a backstop;
  - `http::Client::recvChunkHeader()` caps the chunk-size line at 4 KiB and
    turns a malformed size into an error.
* **Network source** (built, not loaded on Android). The TCP/SCTP block,
  `samplerate / 200` from a user field clamped only from below, is clamped to
  `[1, capacity()]`, and the UDP read is sized from `capacity()`.
* **Hermes** ignores packets shorter than the 1032-byte Metis USB packet.
* **QMX server** checks the packet length without an `assert` that a bad
  server could use to abort a debug build.
* **rtl_tcp** (block from its own rate list, ≤ 16 000) and **KiwiSDR** (exact
  512-sample frames; the WebSocket parser caps frame size) are bounded by
  construction.

Remote sources whose block size the *server* sets split their output to the
stream's `capacity()`:

* **SpyServer** rejects a `BodySize` above `SPYSERVER_MAX_MESSAGE_BODY_SIZE`
  and disconnects, so nothing larger than its fixed 1 MiB `readBuf` is read.
  It publishes each message in pieces of at most `capacity()`, since a full
  8-bit message is 524 288 samples.
* **SDRIAK server client.** Its packet and decompression limits are a wire
  constant, `SERVER_PROTOCOL_MAX_BLOCK` (1 000 000, `server_protocol.h`), not
  the local constant, so a 32-bit Android client accepts a desktop server's
  full-size blocks. `SampleStreamDecompressor` splits each packet to
  `capacity()`, which also covers I16 and I8 packets decoding to two or four
  times as many samples as a same-size F32 one. A server-pushed samplerate
  must be finite and in (0, 64 MS/s], and the user's prebuffer duration is
  capped at 1000 ms, so their product places a finite upper bound on the
  prebuffer ring size.

### 4.2 Blocks that expand their input

`PolyphaseResampler::process()` (`polyphase_resampler.h`) writes
`out[outCount++]` without knowing the output capacity. The input side is only
an `assert` in debug builds. Everything that interpolates goes through it:
`RationalResampler`, `RRCInterpolator` and any `RxVFO` asked for an output
rate above its input rate, which nothing rejects.

The live case is the radio's audio resampler (`radio_module.h`). At a 192 kHz
sink:

    demod        AF rate    expansion   largest safe AF block at 262 144
    -----------  --------   ---------   --------------------------------
    CW/CWR          3 000         64x                              4 096
    AM             15 000       12.8x                             20 480
    USB/LSB/DSB    24 000          8x                             32 768
    NFM            50 000       3.84x                             68 266
    WFM           250 000      0.768x                         decimates

It works because the VFO in front shrinks the block by about as much as the
audio stage expands it (a 131 072-sample HackRF block at 2 MHz becomes
~12 600 audio samples). Nothing enforces that. Radiosonde (up to 4.8x) relies
on the same thing. So chunking a source only to the next stream's capacity is
not enough: the limit has to account for downstream expansion, or the
expanding block has to fragment its output.

### 4.3 Producers that track the constant, or are fixed

Fixed block sizes (largest first):

    hackrf 131 072   airspy 65 536   hydrasdr 65 536   rtl_sdr 7 936
    kcsdr 4 096      airspyhf 4 096  perseus <= 2 048
    qmxserver 512    kiwisdr 512     qmx 512

Rate-dependent, at each device's top rate before the `capacity()` clamp of
§4.1: usrp 1 250 000, sddc 640 000, limesdr 307 200, fobossdr 125 000,
sdrplay 50 000, rtl_tcp 16 000, rfspace ~6 200, hermes 1 982, audio 960.
**bladerf** reads its maximum rate from the device, and **soapy** depends on
the backend; both are clamped.

Self-limiting by their own checks: `plutosdr`, `spectran`, `harogic`, `rfnm`
and the network source. The SDRIAK server client is sized by the wire protocol
instead (§4.1.1).

On Android, the sources that are both built and loaded peak at HackRF's
131 072. SDDC, USRP, BladeRF, LimeSDR and FobosSDR are not in the Android
build. Network and Spectran HTTP are built but not in the hard-coded module
list. SpyServer *is* loaded.

## 5. Other open issues

* **A few producers clamp against `STREAM_BUFFER_SIZE`, not `capacity()`:**
  PlutoSDR, harogic, rfnm and Spectran (USB). That is correct only while their
  stream is not resized, which none is. Sources, network clients, the SDRIAK
  decompressor and `PrebufferedLink` use `capacity()`.
* **Streams whose readers cannot be stopped cannot be shrunk.**
  `setBufferSize()` frees a buffer a running consumer may hold, and
  `TempStop` stops a block but not its reader. This is why the VFO's own
  output stream is 16 MB while carrying about a tenth of that.
* **Failed reconfiguration.** `TempStop` restarts the block even when the
  reconfiguration threw, so every setter must be transactional (build into
  locals, then commit). Most are. `BroadcastFM::setSamplerate()` is not: it
  grows `rdsOut` first, then updates its sub-objects one by one with no
  rollback. It has no caller.
* **Untested:** the RRC interpolator, large interpolation ratios, and a
  `RationalResampler`'s output bound after a live rate change.
* **RadioModule allocates every optional stage up front**, whether or not it
  runs. See §5.1.

### 5.1 Proposal: allocate RadioModule's optional blocks on demand

Status: proposed, not implemented.

Of the 207.5 MB in §3.3, most belongs to optional stages that are only in use
in some modes or with some settings:

    block                            MB          in use when
    -------------------------------  ----------  ----------------------------------
    nb (noise blanker)               16          USB/LSB/RAW, and user-enabled
    fmnr (IF noise reduction)        16 + 8      WFM/NFM, and user-enabled
    powerSquelch                     16          squelch mode "power"
    ctcss                            16 + 17.5   a CTCSS squelch mode is selected
    resamp (AF resampler)            16 + 14     always, except RAW
    hpf (high-pass)                  16 + 8      user-enabled; not in CW/RAW
    deemp (de-emphasis)              16          WFM/NFM with de-emphasis on
    dummyAudioStream                 16          never carries data
    sink volumeInput + volumeAjust   32          always

A typical WFM session (de-emphasis on, no squelch, no IF noise reduction,
high-pass off) runs 78 MB of this and keeps **~130 MB idle**. No setting
combination needs more than ~175 MB at once: `nb` and `fmnr` are never allowed
in the same mode, and `dummyAudioStream` is only the audio chain's input while
demodulators are being swapped.

**Why freeing is safe here.** The general obstacle in §5 is that a stream's
reader cannot be stopped. That does not apply to a disabled block.
`dsp::chain::disableBlock()` stops it and rewires its neighbours around it, so
its output stream has neither a writer nor a reader (`core/src/dsp/chain.h`).
`enableBlock()` wires it back in before starting it, so its storage can be
allocated first, before anything can touch it.

**Why it matters for Android.** RadioModule builds its members with everything
disabled except `resamp`. `selectDemodByID()` then creates WFM, which is where
the startup allocation peaks, and only after that are the user's settings
applied and blocks such as `deemp` enabled. If disabled blocks held no storage,
WFM's 86 MB peak would land on ~62 MB instead of 207.5 MB:

    startup peak        ~294 MB  ->  ~148 MB
    WFM session held    ~292 MB  ->  ~162 MB

These are at the desktop constant; on 32-bit Android all of them scale by
about 0.26. Both need step 3 below. Without it, construction still briefly
allocates the full 160 MB of streams.

**Steps:**

1. **`dummyAudioStream` at capacity 1.** Call `setBufferSize(1)` in the
   constructor, before the sink starts reading it. Nothing ever writes it.
   Saves 16 MB.
2. **Acquire and release hooks managed by the chain.** Add virtual
   `acquireResources()` / `releaseResources()` to the block class.
   - `chain::addBlock(..., false)` and `disableBlock()` release after stopping
     the block.
   - `enableBlock()` acquires before wiring the block in. If acquiring throws,
     the block stays disabled and the error is logged, so an out-of-memory
     error at a toggle is recoverable.
   - Each block implements the hooks: FMIF, CTCSSSquelch, FIRBlock,
     NoiseBlanker, PowerSquelch, Deemphasis and RationalResamplerBlock.
3. **Let a stream start without buffers**, so a block built disabled never
   allocates its 16 MB. This changes `dsp::stream`, and builds on its
   `capacity()` (§8).

**Risks:**

- **Setters must work on a released block.** This is the main cost.
  `selectDemodByID()` and `setAudioSampleRate()` call `ctcss.setSamplerate()`,
  `hpf.setTaps()`, `deemp.setSamplerate()` and `fmnr.setBins()` whether or not
  the block is enabled. Release must drop storage only and keep configuration,
  and acquire must rebuild from that configuration. Losing filter history is
  fine: a re-enabled block's history is stale anyway.
- **Toggle latency.** Enabling allocates and zeroes 16–33 MB, a few
  milliseconds. Enabling already stops and restarts the chain, so there is a
  gap in the audio regardless.
- **Fragmentation.** Allocations this large are `mmap`ed on bionic and glibc
  and returned to the OS on free, so the churn does not fragment the heap. On
  32-bit a toggle still needs a contiguous 16 MB range, which is the
  recoverable failure above.

The stages after the resampler run at 48 kHz, so their streams also carry only
a fraction of their capacity. That separate saving cannot be taken until §4.2
is solved, because the resampler can expand its input 64x in CW.

## 6. How buffers are sized

Buffers behind a rate change are sized from the producer, not from the
constant. Every such site carries `[RATE-SIZED]`:

    grep -rn '\[RATE-SIZED\]' core/src decoder_modules

Code comments link to §2.6 of the long document, which is the full index.

**Two different bounds:**

* `maxOutputBlock()` — the most samples the producer returns or publishes.
* `maxOutputBuffer()` — the most it may *write* in the destination while doing
  it. This can be much larger. The RDS resampler returns ≤ 20 001 samples but
  writes a 125 000-sample intermediate first. `RxVFOKernel` writes the whole
  full-rate input block before resampling it.

Size a destination from `maxOutputBuffer()` and size whatever reads the result
from `maxOutputBlock()`. Never derive either from the nominal rate ratio:
ratio approximation can round up or pick passthrough, and MM clock recovery
can emit faster than the nominal symbol rate.

**Where they come from:**

    PowerDecimator          its decimation plan, stage by stage
    RationalResampler       calculateRatePlan(), the same code reconfigure()
                            applies
    RxVFOKernel             the resampler's bounds, plus the full-rate xlator
    MM clock recovery       minimum legal phase advance after clamping

Static previews (`maxOutputBlockForRates()`, `maxOutputBufferForRates()`,
`RxVFOKernel::max*ForInSamplerate()`) give the same answer for a rate not yet
applied.

`RationalResampler` holds only the stages its current plan runs. A stage the
plan skips is released (`PowerDecimator` at ratio 1, or
`PolyphaseResampler::release()`), including at `init()`.

**Rules for reconfiguration:**

1. Grow the destination from the preview *before* changing the producer, and
   never shrink it there. A throw then leaves the buffer too big, never too
   small.
2. A capacity change is not a signal discontinuity. `setMaxBlock()` keeps
   history. Reset filter history only when the sample spacing really changes.
3. A failed resampler reconfiguration emits nothing (`Mode::DROP`), never
   passthrough, which would overrun a rate-sized destination.

None of these buffers is checked at the moment of writing. A wrong bound on a
scratch or workspace buffer is a silent overflow; on a stream, `swap()` catches
it only after the write. Derive bounds only from the producer's own plan.

## 7. Rules for new producers

* Derive the block size with `dsp::sourceBlockSize(sr, out)`, which clamps
  `sr / 200` to `[1, out.capacity()]`. A bare `sr / 200` overflows as soon as
  the rate is large (§4.1). If you accumulate packets into `writeBuf`, publish
  before the next one would not fit, as Hermes and RFspace do.
* Validate any length that comes from a library or a peer before copying, as
  `spectran` and `qmxserver` do. Check it is not *too small* as well:
  `length - header` on a short value underflows (RFspace in §4.1.1).
* A block whose output can exceed its input must check output capacity
  *before* writing.
* Own every allocation with `Buffer<T>`, `HistoryBuffer<T>`, `tap<T>`,
  `PolyphaseBank<T>`, `FFTWBuffer<T>` or `FFTWPlan`, never a raw pointer.
  They free on throw and keep the old storage if a reallocation fails.

Prefer the destination's `capacity()` to the global constant: streams can be
resized. A producer whose block size a peer controls must split its output to
`capacity()`, as SpyServer and `SampleStreamDecompressor` do.

## 8. The 32-bit Android value, and going further

**32-bit Android uses 262 144** (`#if defined(__ANDROID__) &&
!defined(__LP64__)` in `stream.h`). Why that value is safe there:

* **Local sources.** The largest block any source built and loaded on
  Android writes is HackRF's 131 072, which gives 2x margin. Airspy and
  HydraSDR write 65 536; PlutoSDR clamps itself to the constant; everything
  else writes 16 000 or less. 131 072 would have zero margin: a resampler
  near 1:1 can emit one sample more than it takes in.
* **Audio.** The Android audio sink is fixed at 48 kHz. Through the VFO and
  the audio resampler, the largest audio block is about the source block's
  duration × 48 kHz. That is ~3 150 samples for HackRF at 2 MS/s, the longest
  block, even with CW's 64x expansion.
* **Remote sources.** SpyServer and the SDRIAK server client split their
  output to `capacity()` (§4.1.1).
* **FFT.** FFT sizes up to 524 288 exceed the constant, but no stream carries
  an FFT frame: `IQFrontEnd`'s FFT sink windows its input straight into the
  FFTW buffer, so the FFT size is bounded only by that allocation.

Expected effect, scaled from §3: a radio session peaks at ~77 MB instead of
~294 MB, before counting the VFO, front end and waterfall, which shrink too.
**It needs verifying on an `armeabi-v7a` device**, with HackRF and Airspy in
WFM and CW, and with SpyServer and SDRIAK server connections.

Needed before going lower on 32-bit (131 072 or 65 536), or lowering it on
other platforms:

1. **Make expanding blocks fragment:** resamplers, RRC and the VFO predict
   their output and must split when it does not fit the destination. Add the
   missing tests from §5.
2. **Split fixed-size library callbacks** larger than the stream. HackRF
   (131 072), Airspy and HydraSDR (65 536) and the other fixed sources in
   §4.3 copy whatever the library hands them, so they are not clamped by
   `sourceBlockSize()`. That is safe at 262 144; below it, HackRF must split.
   Also audit dragonlabs.
3. **Count the whole process**, including the VFO, front end and waterfall.
   Independently of this order, steps 1–2 of §5.1 (idle RadioModule blocks)
   can land at any time; step 3 of §5.1 builds on `capacity()`.
4. **Lower further and verify on the device.** On other platforms the
   rate-derived sources follow the constant automatically. A lower value
   there also needs item 2, for HackRF-sized callbacks.
