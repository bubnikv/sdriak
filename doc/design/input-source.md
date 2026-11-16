# Input sources

2026-11-15. The sample rates every source module offers, the block each one
publishes into its output stream at each rate, and the buffers between the
device and that stream. Also what `SampleFrameBuffer` (the IQ front end's input
ring) does, how long each source's driver can absorb a DSP stall on its own, and
what should replace the ring.

Figures are read from the code of the modules and of the driver sources vendored
in the tree (`deps/build-default-*/dep_*`, `source_modules/sddc_source/libsddc`,
`source_modules/qmx_source/libqmx`). Nothing here is measured. "Closed" marks a
vendor library whose source is not in the tree.

---

## 1. Terms

* **Block**: the sample count of one `swap()` on the source's output stream,
  which is what `SampleFrameBuffer`, the front end and every VFO see as one
  packet. "sr/200" means a 5 ms block.
* **Duration**: block ÷ sample rate. **Swaps/s**: its inverse.
* **Wire**: bytes per complex sample on the USB or network link, and the
  resulting link rate. Device-internal formats are not counted.
* **Capacity**: `stream.capacity()`, `STREAM_BUFFER_SIZE` for every source
  stream: 1 000 000 samples, 262 144 on 32-bit Android. `sourceBlockSize()`
  clamps to it (`core/src/dsp/stream.h`). No source stream is resized, so a
  module that clamps to `STREAM_BUFFER_SIZE` gets the same bound.

---

## 2. Sample rates and blocks of every source

### 2.1 Summary

    source            rates offered                  wire       block                     duration
    ----------------  -----------------------------  ---------  ------------------------  ---------------
    HackRF            2-20 MS/s, 7 rates             2 B (u8)   131 072 fixed             6.6-66 ms
    Airspy            device list (10/2.5, 6/3)      4 B        65 536 fixed              6.6-26 ms
    AirspyHF+         device list (912-192 k)        4 B        4 096 fixed               4.5-21 ms
    HydraSDR          library list                   4 B        65 536 / host decimation  6.6-26 ms
    RTL-SDR           250 k-3.2 MS/s, 11 rates       2 B        sr/400                    2.5 ms
    Perseus           48 k-2 MS/s, 10 rates          6 B        1 024 or 2 048            1.0-21 ms
    SDRplay           2-10 MS/s; low-IF 2M/1M/500k   closed     sr/200                    5 ms
    bladeRF           80 k-40 M / 521 k-61.44 M      4 B        sr/200, 1 024-aligned     3.9-12.8 ms
    LimeSDR           100 k-61.44 M (Mini 30.72 M)   4 B        sr/200                    5 ms
    PlutoSDR          1-61.44 MS/s, 0.5 M steps      4 B        sr/20, capped             16-50 ms
    KCSDR             5-40 MS/s, 8 rates             4 B        4 096 fixed               0.1-0.8 ms
    RFNM              61.44 / 122.88 MS/s            3 B        9 or 18 x 32 768          4.8 ms
    SDDC              4-64 MS/s, 5 rates             4 B        sr/200 (after DDC)        5 ms
    FobosSDR          1.25-80 MS/s, 13 rates         4 B        sr/400 at the device rate 0.06-2.5 ms
    Harogic           native / 2^i, 8 rates          1 or 2 B   ~5 ms of packets          ~5 ms
    Spectran (USB)    clock / 1..512                 closed     one API packet            closed
    Dragonlabs        12.5 MS/s fixed                closed     one library callback      closed
    USRP              UHD ranges                     4 B        sr/200                    5 ms
    Soapy             backend list                   backend    sr/200                    5 ms
    QMX (USB audio)   48 kHz fixed                   6 B        512                       10.7 ms
    Audio source      device list                    8 B        sr/200 requested          ~5 ms
    File source       from the file header           file       sr/200                    5 ms, unpaced
    SpyServer         server max / 2^i               2/4/8 B    server message            server-set
    rtl_tcp           250 k-3.2 MS/s, 11 rates       2 B        sr/200                    5 ms
    SDRIAK server     the server's source rate       2/4/8 B    10 ms slices (prebuffer)  10 ms
    KiwiSDR           12 kHz fixed                   4 B        10 ms slices (prebuffer)  10 ms
    Hermes            48/96/192/384 kHz              8 B        sr/200 + < 1 frame        5 ms
    RFspace           38.4 k-1.2288 MS/s             4 B        sr/200 + < 1 datagram     >= 5 ms
    Network source    user entry, >= 1 kS/s          2/4/8/8 B  TCP sr/200; UDP datagram  5 ms / sender
    QMX server        server max / 2^i               2 B        512                       10.7 ms at 48 k
    Spectran HTTP     server rate (5.75 MS/s def.)   8 B        HTTP chunk, split         server-set

Extremes worth knowing:

* **KCSDR** publishes a fixed 4 096 samples. At 40 MS/s that is 9 766 swaps/s
  through the front end and every VFO. The code carries `sampleRate / 200` as a
  comment next to the constant.
* **FobosSDR** below 50 MS/s reads sr/400 samples at the 50 MS/s device rate:
  16 276 reads/s at 1.25 MS/s, and 77-sample blocks out of the DDC.
* **Perseus** at 2 MS/s publishes 2 048 samples every 1.02 ms.
* **RTL-SDR** publishes 2.5 ms blocks at every rate (§2.2.5).
* **PlutoSDR** is the only source that fills the stream: 1 000 000 samples from
  20 MS/s up, 262 144 from 5.24 MS/s up on 32-bit Android.
* **HackRF** publishes the largest fixed block, 131 072 samples (1 MB as
  complex float), at every rate.

### 2.2 USB and local sources

#### 2.2.1 HackRF

`source_modules/hackrf_source/src/main.cpp`. Rates from the module's table.
libhackrf delivers 4 transfers of 262 144 bytes (`TRANSFER_COUNT`,
`TRANSFER_BUFFER_SIZE`): 131 072 samples of 8-bit I/Q. The callback converts one
transfer into the stream and swaps it.

    rate        block      duration   swaps/s   wire
    20 MS/s     131 072      6.55 ms     153    40.0 MB/s
    16 MS/s     131 072      8.19 ms     122    32.0 MB/s
    10 MS/s     131 072     13.11 ms      76    20.0 MB/s
     8 MS/s     131 072     16.38 ms      61    16.0 MB/s
     5 MS/s     131 072     26.21 ms      38    10.0 MB/s
     4 MS/s     131 072     32.77 ms      31     8.0 MB/s
     2 MS/s     131 072     65.54 ms      15     4.0 MB/s

#### 2.2.2 Airspy

`source_modules/airspy_source/src/main.cpp`. Rates from `airspy_get_samplerates()`:
10 and 2.5 MS/s on the R2, 6 and 3 MS/s on the Mini. Default sample type
`AIRSPY_SAMPLE_FLOAT32_IQ`; packing is never enabled. Each 262 144-byte USB
transfer carries 131 072 real 12-bit samples in 16 bits, which the library
converts to 65 536 complex samples on its consumer thread. 16 USB transfers in
flight; an 8-buffer queue between the USB thread and the consumer.

    rate        block     duration   swaps/s   wire
    10 MS/s     65 536      6.55 ms     153    40.0 MB/s
     6 MS/s     65 536     10.92 ms      92    24.0 MB/s
     3 MS/s     65 536     21.85 ms      46    12.0 MB/s
    2.5 MS/s    65 536     26.21 ms      38    10.0 MB/s

#### 2.2.3 AirspyHF+

`source_modules/airspyhf_source/src/main.cpp`. Rates from the firmware
(`airspyhf_get_samplerates()`), typically 912, 768, 456, 384, 256 and 192 kS/s.
Each transfer is `SAMPLES_TO_TRANSFER` = 4 096 complex int16 samples
(16 384 bytes). 16 USB transfers; 8-buffer consumer queue.

    rate        block   duration   swaps/s   wire
    912 kS/s    4 096    4.49 ms      223    3.6 MB/s
    768 kS/s    4 096    5.33 ms      188    3.1 MB/s
    456 kS/s    4 096    8.98 ms      111    1.8 MB/s
    384 kS/s    4 096   10.67 ms       94    1.5 MB/s
    256 kS/s    4 096   16.00 ms       62    1.0 MB/s
    192 kS/s    4 096   21.33 ms       47    768 kB/s

#### 2.2.4 HydraSDR

`source_modules/hydrasdr_source/src/main.cpp`. Rates from
`hydrasdr_get_samplerates()`. The library decimates on the host: hardware rates
are 10, 5 and 2.5 MS/s, and a lower requested rate runs the hardware at one of
them. "Low bandwidth" mode (default) picks the lowest hardware rate that works,
"high definition" the highest. Same transfer geometry as libairspy (256 KB USB
buffers, 16 transfers, 8-buffer queue), so each callback delivers
65 536 / decimation samples, one buffer's worth of time at the hardware rate.

    hardware rate   buffer duration   block at decimation 1 / 4 / 16
    10 MS/s         6.55 ms           65 536 / 16 384 / 4 096
    5 MS/s          13.11 ms          65 536 / 16 384 / 4 096
    2.5 MS/s        26.21 ms          65 536 / 16 384 / 4 096

Wire: 4 bytes per sample at the hardware rate, 10–40 MB/s.

#### 2.2.5 RTL-SDR

`source_modules/rtl_sdr_source/src/main.cpp`. Rates from the module's table.
`asyncCount = round(sr / (200 * 512)) * 512` is passed to `rtlsdr_read_async()`
as the transfer length in **bytes**. At 2 bytes per sample that is sr/400
samples, half of the evident 5 ms target. `buf_num` 0 selects librtlsdr's
default of 15 transfers.

    rate          block   duration   swaps/s   wire       15 transfers
    250 kS/s        512    2.05 ms      488    500 kB/s     31 ms
    1.024 MS/s    2 560    2.50 ms      400    2.0 MB/s     38 ms
    1.536 MS/s    3 840    2.50 ms      400    3.1 MB/s     38 ms
    1.792 MS/s    4 608    2.57 ms      389    3.6 MB/s     39 ms
    1.92 MS/s     4 864    2.53 ms      395    3.8 MB/s     38 ms
    2.048 MS/s    5 120    2.50 ms      400    4.1 MB/s     38 ms
    2.16 MS/s     5 376    2.49 ms      402    4.3 MB/s     37 ms
    2.4 MS/s      5 888    2.45 ms      408    4.8 MB/s     37 ms
    2.56 MS/s     6 400    2.50 ms      400    5.1 MB/s     38 ms
    2.88 MS/s     7 168    2.49 ms      402    5.8 MB/s     37 ms
    3.2 MS/s      7 936    2.48 ms      403    6.4 MB/s     37 ms

librtlsdr's own default transfer (`DEFAULT_BUF_LENGTH` = 262 144 bytes) would
be 131 072 samples, 55 ms at 2.4 MS/s.

#### 2.2.6 Perseus

`source_modules/perseus_source/src/main.cpp`. Rates from libperseus's FPGA image
table (`perseus_get_sampling_rates()`). Block =
`clamp((sr/200) / 1024, 1, 2) × 1024`, so 1 024 or 2 048 samples. 24-bit I/Q,
6 bytes per sample; libperseus allows at most 16 320 bytes per transfer and
keeps 8 transfers.

    rate        block   duration   swaps/s   wire        8 transfers
    48 kS/s     1 024   21.33 ms       47    288 kB/s     171 ms
    95 kS/s     1 024   10.78 ms       93    570 kB/s      86 ms
    96 kS/s     1 024   10.67 ms       94    576 kB/s      85 ms
    125 kS/s    1 024    8.19 ms      122    750 kB/s      66 ms
    192 kS/s    1 024    5.33 ms      188    1.2 MB/s      43 ms
    250 kS/s    1 024    4.10 ms      244    1.5 MB/s      33 ms
    500 kS/s    2 048    4.10 ms      244    3.0 MB/s      33 ms
    1 MS/s      2 048    2.05 ms      488    6.0 MB/s      16 ms
    1.6 MS/s    2 048    1.28 ms      781    9.6 MB/s      10 ms
    2 MS/s      2 048    1.02 ms      977   12.0 MB/s       8 ms

#### 2.2.7 SDRplay

`source_modules/sdrplay_source/src/main.cpp`. Closed API. Zero-IF rates 2–10 MS/s
in 1 MS/s steps. Low-IF modes run the device at 8, 6 or 2 MS/s and deliver
2 MS/s, 1 MS/s or 500 kS/s (`ifModes[]`). The API calls `streamCB()` with
whatever `numSamples` it chooses; the module accumulates exactly
`bufferSize = sr/200` samples and swaps, mid-callback if needed.

    rate                 block    duration   swaps/s
    2-10 MS/s (zero-IF)  10 000-50 000   5 ms   200
    2 MS/s low-IF        10 000          5 ms   200
    1 MS/s low-IF         5 000          5 ms   200
    500 kS/s low-IF       2 500          5 ms   200

The API delivers int16 I/Q; the USB format is internal to the API service.

#### 2.2.8 bladeRF

`source_modules/bladerf_source/src/main.cpp`. The list is the device minimum,
then 2 MS/s steps, then the maximum (`bladerf_get_sample_rate_range()`):

* bladeRF 1: 80 kS/s – 40 MS/s (`BLADERF_SAMPLERATE_MIN`,
  `BLADERF_SAMPLERATE_REC_MAX`).
* bladeRF 2: 520 834 S/s – 122.88 MS/s as reported, but rates above 61.44 MS/s
  need the oversample feature, which the module never enables. The module
  ignores `bladerf_set_sample_rate()`'s result, so selecting one of those fails
  silently.

Block = sr/200 rounded down to a multiple of 1 024, at least 1 024. SC16_Q11,
4 bytes per sample. `bladerf_sync_config()`: 16 buffers of one block,
8 transfers.

    rate          block      duration   swaps/s   wire
    80 kS/s         1 024    12.80 ms       78    320 kB/s
    520.8 kS/s      2 048     3.93 ms      254    2.1 MB/s
    2 MS/s          9 216     4.61 ms      217    8.0 MB/s
    10 MS/s        49 152     4.92 ms      203    40.0 MB/s
    20 MS/s        99 328     4.97 ms      201    80.0 MB/s
    40 MS/s       199 680     4.99 ms      200    160.0 MB/s
    61.44 MS/s    307 200     5.00 ms      200    245.8 MB/s

#### 2.2.9 LimeSDR

`source_modules/limesdr_source/src/main.cpp`. The list is the device minimum,
then 1 MS/s steps, then the maximum (`LMS_GetSampleRateRange()`): LimeSDR USB
100 kS/s – 61.44 MS/s, LimeSDR Mini 100 kS/s – 30.72 MS/s. The F32 stream uses
an int16 link (4 bytes per sample). Block = sr/200, the count passed to
`LMS_RecvStream()`. The host FIFO is `fifoSize` = sr/200, which LimeSuite
raises to at least 4 packets of 1 020 samples (4 080).

    rate          block      duration   host FIFO   wire
    100 kS/s          500     5 ms        4 080     400 kB/s
    1 MS/s          5 000     5 ms        5 000     4.0 MB/s
    10 MS/s        50 000     5 ms       50 000     40.0 MB/s
    30.72 MS/s    153 600     5 ms      153 600     122.9 MB/s
    61.44 MS/s    307 200     5 ms      307 200     245.8 MB/s

#### 2.2.10 PlutoSDR

`source_modules/plutosdr_source/src/main.cpp`. USB or IP (libiio scans
`usb:ip`; Android connects to `ip:192.168.2.1`). Rates 1–61.44 MS/s in
0.5 MS/s steps. Block = `min(sr/20, STREAM_BUFFER_SIZE)`, the libiio buffer
length; 8 kernel buffers. int16 I/Q, 4 bytes per sample.

    rate          block       duration   swaps/s   wire
    1 MS/s          50 000    50.00 ms       20    4.0 MB/s
    4 MS/s         200 000    50.00 ms       20    16.0 MB/s
    10 MS/s        500 000    50.00 ms       20    40.0 MB/s
    20 MS/s      1 000 000    50.00 ms       20    80.0 MB/s
    30 MS/s      1 000 000    33.33 ms       30    120.0 MB/s
    61.44 MS/s   1 000 000    16.28 ms       61    245.8 MB/s

On 32-bit Android the cap is 262 144: 26.2 ms at 10 MS/s, 13.1 ms at 20 MS/s.

#### 2.2.11 KCSDR

`source_modules/kcsdr_source/src/main.cpp`. Closed library. Rates 40, 35, 30,
25, 20, 15, 10 and 5 MS/s. `kcsdr_rx()` reads a fixed `0x4000/4` = 4 096
samples of int16 I/Q, and each read is one swap.

    rate       block   duration   swaps/s   wire
    40 MS/s    4 096    0.10 ms    9 766    160.0 MB/s
    30 MS/s    4 096    0.14 ms    7 324    120.0 MB/s
    20 MS/s    4 096    0.20 ms    4 883     80.0 MB/s
    10 MS/s    4 096    0.41 ms    2 441     40.0 MB/s
     5 MS/s    4 096    0.82 ms    1 221     20.0 MB/s

#### 2.2.12 RFNM

`source_modules/rfnm_source/src/main.cpp`. Rates 61.44 MS/s (decimation 2) and
122.88 MS/s (decimation 1). The USB link carries 12-bit packed I/Q (3 bytes per
sample) in packets of 32 768 samples (`RFNM_USB_RX_PACKET_ELEM_CNT`), which
librfnm unpacks to CS16. The module queues `LIBRFNM_MIN_RX_BUFCNT` = 1 000
buffers of 131 072 bytes (131 MB of host memory) and swaps
`k = (sr / 32 768) / 200` packets at a time, at most
`STREAM_BUFFER_SIZE / 32 768` (30 on desktop, 8 on 32-bit Android).

    rate           packets per swap   block     duration   wire
    61.44 MS/s      9                 294 912    4.80 ms    184.3 MB/s
    122.88 MS/s    18                 589 824    4.80 ms    368.6 MB/s

#### 2.2.13 SDDC

`source_modules/sddc_source/src/main.cpp`, `libsddc/src/sddc.c`. Rates 4, 8, 16,
32 and 64 MS/s. The ADC runs at twice the rate and delivers real int16 samples.
Each `sddc_rx()` is one synchronous `libusb_bulk_transfer()` of
`bufferSize = sr/100` real samples (5 ms). The module copies them into `ddcIn`
as complex samples with zero imaginary part, and an `RxVFO` DDC halves the rate
and publishes `ddc.out`.

    rate      ADC rate   read (real)   bulk transfer   block out   wire
    4 MS/s     8 MS/s      40 000         80 KB          20 000    16 MB/s
    8 MS/s    16 MS/s      80 000        160 KB          40 000    32 MB/s
    16 MS/s   32 MS/s     160 000        320 KB          80 000    64 MB/s
    32 MS/s   64 MS/s     320 000        640 KB         160 000   128 MB/s
    64 MS/s  128 MS/s     640 000       1.28 MB         320 000   256 MB/s

#### 2.2.14 FobosSDR

`source_modules/fobossdr_source/src/main.cpp`. Device rates from libfobos (80,
50, 40, 32, 25, 20, 16, 12.5, 10, 8 MS/s) plus three DDC-only rates (5, 2.5,
1.25 MS/s). The device runs at `max(rate, 50 MS/s)`. So on the RF port only 80
and 50 MS/s bypass the DDC; every other rate, and every rate on the HF ports
(where the DDC also shifts the frequency), goes through an `RxVFO` DDC from the
device rate. Sync mode: each `fobos_rx_read_sync()` is one synchronous bulk
transfer of `bufferSize = sr/400` samples (rounded down to 128 by libfobos) at
the **device** rate. int16 I/Q, 4 bytes per sample.

    rate        device    read      read time   reads/s   block out
    80 MS/s     80 MS/s   199 936    2.50 ms        400     199 936
    50 MS/s     50 MS/s   124 928    2.50 ms        400     124 928
    40 MS/s     50 MS/s    99 968    2.00 ms        500     ~79 974
    20 MS/s     50 MS/s    49 920    1.00 ms      1 002     ~19 968
    10 MS/s     50 MS/s    24 960    0.50 ms      2 003      ~4 992
    8 MS/s      50 MS/s    19 968    0.40 ms      2 504      ~3 195
    5 MS/s      50 MS/s    12 416    0.25 ms      4 027      ~1 242
    2.5 MS/s    50 MS/s     6 144    0.12 ms      8 138        ~307
    1.25 MS/s   50 MS/s     3 072    0.06 ms     16 276         ~77

The link runs at 200 MB/s for every rate below 50 MS/s, and at 320 MB/s at
80 MS/s.

#### 2.2.15 Harogic

`source_modules/harogic_source/src/main.cpp`. Closed API. Rates = the profile's
`NativeIQSampleRate_SPS` / 2^i for i = 0…7 (the module's default is
61.44 MS/s). The API reports `PacketSamples` per packet; the module swaps
`k = (sr / PacketSamples) / 200` packets at a time (about 5 ms), at most
`STREAM_BUFFER_SIZE / PacketSamples`. int8 or int16 I/Q.

#### 2.2.16 Spectran (USB)

`source_modules/spectran_source/src/main.cpp`. Closed RTSA API. Rates = receiver
clock (92.16, 122.88, 184.32 or 245.76 MHz) / decimation (1 to 512, powers of
two), deduplicated: 180 kS/s to 245.76 MS/s. One swap per RTSA packet
(`pkt.num`, chosen by the API). A packet larger than `STREAM_BUFFER_SIZE` is
logged ("Buffer too big") and dropped. USB compression is a user option.

#### 2.2.17 Dragonlabs

`source_modules/dragonlabs_source/src/main.cpp`. Closed library. Fixed
12.5 MS/s. One swap per library callback, `count` chosen by the library. The
callback copies `count` samples into `writeBuf` without checking it against the
stream's capacity, and ignores its `drops` argument.

#### 2.2.18 USRP

`source_modules/usrp_source/src/main.cpp`. Rates enumerated from UHD's
`get_rx_rates()` ranges, stepping 100 kHz where UHD reports a step of 0, which
can give lists of hundreds of entries. Block = sr/200 clamped to capacity, the
count passed to `recv()`; a shorter return is printed and published as is.
`otw_format` sc16 (4 bytes per sample), `cpu_format` fc32. Receive buffer sizes
are UHD's defaults.

#### 2.2.19 Soapy

`source_modules/soapy_source/src/main.cpp`. Rates from `listSampleRates()`.
Block = sr/200 clamped to capacity, the count passed to `readStream()`. CF32
requested; the link format depends on the backend.

#### 2.2.20 QMX (USB audio)

`source_modules/qmx_source/src/main.cpp`, `libqmx`. Fixed 48 kHz
(`qmx::kSampleRate`). Every backend publishes blocks of
`kStreamBlockSize` = 512 samples: 10.67 ms, 93.75 swaps/s. Linux captures ALSA
`S24_3LE` (6 bytes per frame, 288 kB/s), falling back to `S32_LE`, with a
256-frame period and a 1 024-frame buffer. Windows uses WASAPI exclusive mode at
the device period; Android uses 5 isochronous transfers of 20 × 1 ms packets;
macOS uses an AudioUnit capture callback.

#### 2.2.21 Audio source

`source_modules/audio_source/src/main.cpp`. Rates from the RtAudio device.
Stereo float32 is taken as I/Q (8 bytes per sample). The requested period is
sr/200 (220 at 44.1 kHz, 240 at 48 kHz, 480 at 96 kHz, 960 at 192 kHz); RtAudio
may choose another, and the callback splits a period larger than the stream.

#### 2.2.22 File source

`source_modules/file_source/src/main.cpp`. Rate from the file header. WAV and
RF64 with PCM u8, 16, 24 or 32-bit or IEEE float 32 or 64-bit, and FLAC. Block
= sr/200 clamped to capacity and to 16 MB of file data. Stereo float32 is read
straight into the stream. There is no pacing: it reads as fast as the front end
accepts (§3.2).

### 2.3 Network sources

Only two network sources pace their output with `PrebufferedLink`
(`core/src/dsp/buffer/prebuffer.h`): the SDRIAK server client and KiwiSDR.
Everything else publishes what arrives, as it arrives, with only the socket's
kernel buffer in front.

`PrebufferedLink` releases 10 ms slices on a clock (`TICK_MS`) once the target
depth has filled. Its ring holds `max(8 slices, target + slack)`, where slack is
`max(40 ms, min(target/2, 100 ms))`. With the target at 0 it passes packets
through.

#### 2.3.1 SpyServer

`source_modules/spyserver_source/`. Rates = `MaximumSampleRate / 2^i` for
i from `MinimumIQDecimation` to `DecimationStageCount`, as the server reports.
Formats uint8 (2 bytes per sample), int16 (4) and float32 (8); int24 is decoded
when received. The server chooses the message size; a body above
`SPYSERVER_MAX_MESSAGE_BODY_SIZE` (1 MiB) disconnects. Each message is published
in pieces of at most `capacity()` samples: a full 1 MiB uint8 message is
524 288 samples, two pieces on 32-bit Android.

    format     bytes/sample   wire at 1 MS/s   at 2.5 MS/s   at 10 MS/s
    uint8      2                2 MB/s           5 MB/s        20 MB/s
    int16      4                4 MB/s          10 MB/s        40 MB/s
    float32    8                8 MB/s          20 MB/s        80 MB/s

#### 2.3.2 rtl_tcp

`source_modules/rtl_tcp_source/`. The same 11 rates as RTL-SDR. uint8 I/Q,
2 bytes per sample. Each `recv()` waits for exactly sr/200 samples, which are
then one block.

    rate          block    duration   wire
    250 kS/s       1 250     5 ms     500 kB/s
    1.024 MS/s     5 120     5 ms     2.0 MB/s
    2.048 MS/s    10 240     5 ms     4.1 MB/s
    2.4 MS/s      12 000     5 ms     4.8 MB/s
    3.2 MS/s      16 000     5 ms     6.4 MB/s

#### 2.3.3 SDRIAK server client

`source_modules/sdriak_server_source/`. The rate is that of the source running on
the server, so any rate in §2.2. The server sends one baseband packet per block
of its own source, so the packet sizes are that source's blocks from §2.2, up to
the wire limit `SERVER_PROTOCOL_MAX_BLOCK` (1 000 000).
`SampleStreamDecompressor` splits them to the client stream's `capacity()`.

* **Formats:** int8 (2 bytes per sample), int16 (4, default) and float32 (8),
  optionally zstd level 1.
* **Buffers:** receive and send packet buffers of `SERVER_MAX_PACKET_SIZE`
  (16 MB each); decompressor input 8 MB.
* **Prebuffer:** Disabled, 50, 100 (default), 250, 500 or 1 000 ms. With it on,
  the output is 10 ms slices; at 100 ms the ring holds 150 ms.

#### 2.3.4 KiwiSDR

`source_modules/kiwisdr_source/`, `core/src/utils/proto/kiwisdr.cpp`. Fixed
12 kHz I/Q (`IQDATA_FREQUENCY`). The server sends WebSocket frames of 512
samples of big-endian int16 I/Q (2 048 bytes plus a 20-byte header), 42.7 ms
each, 23.4 frames/s, 48 kB/s; compression is turned off. The prebuffer is
0–2 000 ms, default 250 ms: the output is 10 ms slices of 120 samples, and at
250 ms the ring holds 350 ms (4 200 samples).

#### 2.3.5 Hermes

`source_modules/hermes_source/`. Rates 48, 96, 192 and 384 kHz (default 384).
Each UDP datagram is a 1 032-byte Metis packet holding two USB frames of 63
samples. A sample slot is 8 bytes: 24-bit I, 24-bit Q and 16-bit microphone.
Frames accumulate until sr/200 samples, so a block is sr/200 rounded up to whole
frames.

    rate       block      datagrams/s   wire
    48 kHz      240-302        381      393 kB/s
    96 kHz      480-542        762      786 kB/s
    192 kHz     960-1 022    1 524      1.6 MB/s
    384 kHz   1 920-1 982    3 048      3.1 MB/s

#### 2.3.6 RFspace

`source_modules/rfspace_source/`. Rates depend on the device:

* NetSDR and SDR-IP: 80 MHz / 100 halved down to 32 kS/s: 800, 400, 200, 100
  and 50 kS/s.
* CloudSDR and CloudIQ: 122.88 MHz / 100 halved: 1 228.8, 614.4, 307.2, 153.6,
  76.8 and 38.4 kS/s.

int16 I/Q, 4 bytes per sample. UDP data items of up to 8 194 bytes (at most
2 047 samples), sized by the device. Datagrams accumulate until sr/200 samples,
so a block is sr/200 rounded up to whole datagrams; at the lowest rates one
datagram is already more than 5 ms.

    rate           sr/200   wire
    1 228.8 kS/s    6 144   4.9 MB/s
    800 kS/s        4 000   3.2 MB/s
    307.2 kS/s      1 536   1.2 MB/s
    100 kS/s          500   400 kB/s
    38.4 kS/s         192   154 kB/s

#### 2.3.7 Network source

`source_modules/network_source/src/main.cpp`. Rate is a user entry, at least
1 000 S/s. Sample types int8, int16, int32 and float32 (2, 4, 8 and 8 bytes per
sample). Over TCP each `recv()` waits for exactly sr/200 samples. Over UDP each
datagram is one block, sized by the sender; a datagram's 65 507-byte limit
gives at most 32 753 int8, 16 376 int16 or 8 188 int32 or float32 samples.

#### 2.3.8 QMX server

`source_modules/qmxserver_source/`. ENet over UDP. Rates =
`MaximumSampleRate / 2^i`, as the server reports (48 kHz for a QMX). Sample
packets are `EXT_BLOCKLEN` = 512 int16 samples (2 048 bytes): 10.67 ms at
48 kHz, 192 kB/s. The SpyServer-style message path also accepts uint8, int16
and float32 messages, one block per message.

#### 2.3.9 Spectran HTTP

`source_modules/spectran_http_source/`. The client requests a rate in the
receiver configuration (default 5.75 MS/s) and takes the actual one from each
chunk's metadata. float32 I/Q, 8 bytes per sample: 46 MB/s at 5.75 MS/s. The
server chooses the HTTP chunk size; the client reads each chunk in pieces of at
most `capacity()` samples and publishes each piece. The metadata line is capped
at 64 KiB.

### 2.4 Not built

`sdrpp_server_source/` is an empty directory; its module is now
`sdriak_server_source`. `OPT_BUILD_BADGESDR_SOURCE` names
`source_modules/badgesdr_source`, which does not exist.

---

## 3. `SampleFrameBuffer`

`core/src/dsp/buffer/frame_buffer.h`, used once as `IQFrontEnd::inBuf`: the
first stage after the source, in front of the pre-processing chain and the IQ
splitter (`core/src/signal_path/iq_frontend.cpp`).

### 3.1 Behaviour

* **Buffering mode.** `run()` copies each source packet into slot `writeCur` of
  a 32-slot ring and returns at once. It never waits for space. A second thread
  (`readWorkerThread`) swaps the queued packets downstream one at a time, sizes
  unchanged. A downstream stall blocks the second thread, not the source.
* **Bypass mode.** `run()` copies the packet into `out` and swaps it, so the
  source sees ordinary back-pressure. Both threads still run; the second one
  waits forever.
* **Overflow.** When the writer laps the reader, `writeCur == readCur` reads as
  an empty ring, so a whole ring of packets is lost at once. Nothing is logged.

### 3.2 When it is on

* **On** from startup: `MainWindow::init()` builds the front end with
  `buffering = true` (`core/src/gui/main_window.cpp:109`). That covers every
  source.
* **Off** only while `file_source` is selected: `menuSelected()` calls
  `setBuffering(false)` and `menuDeselected()` calls `setBuffering(true)`
  (`source_modules/file_source/src/main.cpp:166`, `:176`). The file source has
  no pacing of its own and would lap the ring. Nothing else calls
  `setBuffering()`.
* The state is not persisted. There is no UI control. The "Bypass buffering"
  checkbox and the occupancy readout in the debug menu
  (`core/src/gui/main_window.cpp:779`) are commented out, sit inside
  `SDRPP_ENABLE_DEBUG_MENU`, and refer to a member name that no longer exists.
* `MainWindow::setPlayState()` flushes the ring when playback starts and stops.
* `sdrpp_server` builds no front end and therefore no ring.

### 3.3 Cost

* **Address space.** 32 × `STREAM_BUFFER_SIZE` complex samples is 256 MB
  (64 MB on 32-bit Android), allocated in `init()` whether or not buffering is
  on. Pages are touched only as they are written, so the resident size settles
  at 32 × the largest block (§2): 32 MB for HackRF, 16 MB for Airspy, 256 MB
  for PlutoSDR at 20 MS/s and above.
* **The `out` stream** is another `STREAM_BUFFER_SIZE` stream (16 MB).
* **Two threads**, both always started.
* **Two full-rate copies** per packet in buffering mode, into the ring and back
  out: 320 MB/s at 20 MS/s.
* **Depth depends on the source.** The ring counts packets, not time, so its
  depth is 32 blocks of §2: 3.2 ms for KCSDR at 40 MS/s, 79 ms for RTL-SDR,
  160 ms for an sr/200 source, 2.1 s for HackRF at 2 MS/s, 1.6 s for PlutoSDR.

### 3.4 Defects

* `setBuffering()` writes the plain `bool bypass` while `run()` reads it.
* If buffering is switched off while packets are queued, `run()` (bypass) and
  `readWorkerThread` both `memcpy` into `out.writeBuf` and both call `swap()`.
  Selecting the file source does exactly that.
* **A stall turns into permanent latency.** When a radio plays audio, the audio
  device paces the chain: `Packer::swap()` blocks on the device callback, and
  that back-pressure reaches the IQ splitter. After a stall the ring cannot
  drain faster than real time, so the backlog stays as latency, up to the ring's
  depth.
* **Clock drift.** If the SDR's clock runs fast against the sound card's, the
  ring fills slowly and then laps, losing a ring's worth of packets in one go.
  Without the ring, the same excess would be dropped in small pieces at the
  driver.

### 3.5 Origin

Upstream SDR++, `dff9ec2d` "new buffer thingy" (2021-06-28). That commit also
added the debug-menu checkbox and occupancy readout; the slot count is named
`TEST_BUFFER_SIZE`. Bypass and the file-source toggle came in `aaa15315`
(2021-06-29). The 2022 DSP rewrite (`d1318d3a`) added the comment
"THIS IS TRASH AND MUST BE REWRITTEN IN THE FUTURE".

No upstream issue or pull request states the purpose. The search covered
closed issues and PRs for buffering, `SampleFrameBuffer`, input buffer, frame
buffer, and lost or dropped samples, plus stutter, choppy, underrun and
overflow reports from April to August 2021. GitHub Discussions were not
searchable.

* **#131**, "Audio skipping but recording is fine", opened four weeks before
  the buffer, is the closest candidate. RTL-SDR over USB at 2.4 MS/s had audio
  dropouts, and over `rtl_tcp` it did not. It was attributed to RtAudio's ALSA
  backend and resolved by building RtAudio with PulseAudio forced.
* **#266**, the same kind of report seven weeks after the buffer landed,
  received the same answer.
* Upstream treats network jitter buffering as separate, unbuilt work (#846,
  #1713).

---

## 4. What each source does while its thread is blocked

The thread that calls `swap()` on the source stream blocks whenever the front
end has not flushed the previous packet. What that costs depends on the driver.

* **Pattern A: the driver keeps receiving.** It has its own queue, or reception
  does not depend on our thread. A stall is harmless until that queue fills.
  Then the driver drops data, usually counting it.
* **Pattern B: reception waits for our thread.** Either the callback runs on the
  USB event thread and the transfer is resubmitted only after it returns, or each
  read is one synchronous bulk transfer. While `swap()` blocks, only the
  transfers already submitted, or the device's own FIFO, take data. When they
  are full, the device overruns and samples are lost without any report.

**Tolerance** is how long the DSP chain can stall, with the ring off, before
samples are lost. It counts host-side buffering only; the devices' own FIFOs add
a little and are not counted.

### 4.1 USB and local sources

    source       pattern  what is in flight or queued                     tolerance
    -----------  -------  ----------------------------------------------  ------------------------------
    SDDC         B        one synchronous libusb_bulk_transfer() per      ~1 block (5 ms)
                          read; the DDC thread gives one block of slack
    FobosSDR     B        sync mode: one synchronous bulk transfer per    ~1 read (0.06-2.5 ms, §2.2.14);
                          fobos_rx_read_sync()                            less when the DDC is bypassed
    KCSDR        closed   fixed 4 096-sample reads                        not determinable
    LimeSDR      A        LimeSuite host FIFO of max(sr/200, 4 080)       ~5 ms (41 ms at 100 kS/s)
                          samples
    Perseus      B        libperseus: 8 transfers of 1 024-2 048          8 ms at 2 MS/s, 171 ms at 48 k
                          samples, resubmitted after the callback
    QMX          B        Linux: ALSA buffer of 1 024 frames.             ~21 ms (Linux), a few ms
                          Windows: WASAPI exclusive, one device period.   (Windows), ~80 ms (Android)
                          Android: 5 isochronous transfers x 20 ms
    RTL-SDR      B        librtlsdr async: 15 transfers of sr/400         ~35 ms at every rate
                          samples each
    HackRF       B        libhackrf: 4 transfers x 131 072 samples; the   197 ms at 2 MS/s, 39 ms at
                          callback runs under transfer_lock               10 MS/s, 20 ms at 20 MS/s
    AirspyHF+    A        own consumer thread; queue of 8 x 4 096         43 ms at 768 k, 171 ms at 192 k
                          samples; 16 USB transfers keep running
    Airspy       A        own consumer thread; queue of 8 x 65 536        52 ms at 10 MS/s, 210 ms at
                          samples; 16 USB transfers keep running          2.5 MS/s
    HydraSDR     A        same structure as libairspy (8 x 256 KB         about the same as Airspy
                          queue, 16 transfers)
    bladeRF      A        sync API: 16 buffers of 5 ms, 8 transfers       ~80 ms
    PlutoSDR     A        libiio: 8 kernel buffers x sr/20                ~400 ms (less above 20 MS/s,
                                                                          where the block is capped)
    RFNM         A        librfnm: 1 000 queued buffers of 32 768         ~530 ms at 61.44 MS/s
                          samples, 16 USB threads
    USRP         A        UHD defaults; the module sets no recv buffer    not determinable here
                          size
    Soapy        A        backend-dependent                               not determinable here

Closed libraries, not determinable: SDRplay (callback from the API service's
thread), KCSDR, Harogic, Dragonlabs, Spectran. Spectran pulls packets from the
RTSA service's own queue.

Where the code is:

* HackRF: `source_modules/hackrf_source/src/main.cpp` `callback()`;
  libhackrf `hackrf_libusb_transfer_callback()`, `TRANSFER_COUNT 4`,
  `TRANSFER_BUFFER_SIZE 262144`.
* Airspy, AirspyHF+, HydraSDR: libairspy `airspy_libusb_transfer_callback()`
  and `consumer_threadproc()`, `RAW_BUFFER_COUNT 8`; libairspyhf
  `SAMPLES_TO_TRANSFER 4096`; libhydrasdr `hydrasdr_shared.h`.
* RTL-SDR: `source_modules/rtl_sdr_source/src/main.cpp` `worker()` and
  `asyncHandler()`; librtlsdr `_libusb_callback()`, `DEFAULT_BUF_NUMBER 15`.
* Perseus: `source_modules/perseus_source/src/main.cpp`; libperseus
  `perseus_start_async_input()` (queue of 8) and `input_queue_callback()`.
* bladeRF: `bladerf_sync_config(..., 16, bufferSize, 8, ...)` in
  `source_modules/bladerf_source/src/main.cpp`.
* LimeSDR: `devStream.fifoSize` in `source_modules/limesdr_source/src/main.cpp`;
  LimeSuite `StreamChannel::Setup()`.
* PlutoSDR: `iio_device_set_kernel_buffers_count(dev, 8)` in
  `source_modules/plutosdr_source/src/main.cpp`.
* SDDC: `source_modules/sddc_source/libsddc/src/sddc.c` `sddc_rx()`.
* FobosSDR: libfobos `fobos_rx_read_sync()`.
* QMX: `source_modules/qmx_source/libqmx/src/*Backend.cpp`.
* RFNM: `LIBRFNM_MIN_RX_BUFCNT 1000` in `librfnm.h`.

### 4.2 Other sources

* **Network sources.** The kernel's socket buffer absorbs only a finite stall;
  the SDRIAK server client and KiwiSDR also have `PrebufferedLink` (§2.3).
  Once those buffers fill, TCP backpressures the sender and can accumulate
  latency or cause a server-side drop, while UDP sources (Hermes, RFspace, the
  network source over UDP and QMX server) lose datagrams in the kernel.
* **Audio source.** The RtAudio input callback blocks in `swap()`. The capture
  backend supplies its own period buffering, and the front-end ring adds up to
  32 more callback periods. Removing the ring therefore reduces the tolerated
  stall by that amount and needs backend-specific underrun/drop measurements.
* **File source.** It has no pacing and runs with the ring bypassed (§3.2).

### 4.3 Observations

* **RTL-SDR's small tolerance comes from the module**: its 2.5 ms transfers
  (§2.2.5). librtlsdr's default 256 KB transfers would give ~760 ms of
  tolerance at 2.4 MS/s, with 55 ms delivery chunks.
* **Drops are silent almost everywhere.**
  * libairspy, libairspyhf and libhydrasdr report `transfer->dropped_samples`,
    and the modules ignore it.
  * Dragonlabs' callback receives a `drops` argument, also ignored.
  * USRP does not check `meta.error_code` for overflow.
  * Pattern-B drivers (HackRF, RTL-SDR, Perseus, SDDC, Fobos) report nothing.
* **The ring does not cover every stall.** At 20 MS/s on HackRF the ring holds
  ~210 ms. A longer stall laps it and loses the whole ring.
* **Known stall sources.**
  * The FFT sink takes the waterfall's `buf_mtx` in `WaterFall::getFFTBuffer()`
    (`core/src/gui/widgets/waterfall.cpp`), and the GUI thread holds it during
    draw and `onResize()`. A GUI relayout can therefore stall the IQ splitter
    and every VFO.
  * Reconfigurations stop blocks with `TempStop`. A demodulator switch
    allocates tens of MB.

---

## 5. Recommendation

Removing the ring outright would expose SDDC, FobosSDR, LimeSDR, Perseus, QMX
and RTL-SDR to any DSP stall longer than 5–35 ms, which is the size of the
stalls listed in §4.3. Order of work:

1. **Make drops visible.** Log, at a limited rate, `dropped_samples` for the
   Airspy family, `drops` for Dragonlabs and UHD overflows. It costs little and
   gives the measurement that every later step needs.
2. **Raise the tolerance at the driver where it is a setting.**
   * RTL-SDR: larger async transfers, for example 16 ms each, giving ~240 ms.
   * LimeSDR: a real `fifoSize`.
   * FobosSDR: the async API (`fobos_rx_read_async()`, 16 buffers by default),
     with reads sized from the device rate rather than the output rate.
   * KCSDR: reads of sr/200 instead of a fixed 4 096 samples.
3. **Keep a buffer only for sources that cannot be fixed at the driver**: SDDC,
   Perseus (libperseus fixes the queue at 8 transfers of at most 16 320 bytes),
   QMX, and HackRF at high rates. Each source declares whether it needs the
   buffer, applied in `SourceManager::selectSource()`; this replaces the file
   source's own toggling. The buffer becomes a FIFO of samples, sized by time,
   in fixed segments, allocated only while it is on:
   * **Segments** of 65 536 samples (512 KB complex). Packets are split across
     them, and small packets share a segment, so the depth in time is the same
     for every source.
   * **Depth** of `max(sampleRate × 250 ms, 2 × in->capacity())`, capped at
     32 MB, built under `TempStop` on a rate change or when the buffer is
     enabled, never on the worker thread.
   * **Overflow** drops the oldest samples and counts them. That keeps "never
     block the source" while losing only the excess, and brings latency back
     down after a stall.
   * **Mode switches** go through `TempStop`, which removes both defects in
     §3.4. Bypass starts one thread and allocates nothing.
4. **Remove the stalls themselves**, starting with the FFT sink blocking on
   `buf_mtx`: a try-lock that drops the frame, or a double buffer.

Network sources, the audio source and the Pattern-A USB sources can then run
without the front-end ring only after measurements show that their remaining
socket, prebuffer or backend tolerance covers normal DSP stalls. Their 256 MB
of address space and the two full-rate copies then go away.

### 5.1 Tests

* Packets larger and smaller than a segment come out in order, unchanged and
  without duplicates.
* An overflow drops exactly the excess, oldest first, and counts it.
* Switching modes with packets queued leaves exactly one writer on `out`.
* After a stop and `flush()`, nothing stale comes out.
* In bypass no segment is allocated.
* On hardware: RTL-SDR over USB at 2.4 MS/s (the #131 setup) and HackRF at
  20 MS/s, with buffering off, while switching demodulators and resizing the
  waterfall, watching the counters from step 1.
