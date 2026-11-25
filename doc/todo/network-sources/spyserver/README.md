# SpyServer: optional VFO+FFT mode with server waterfall

Date: 2026-11-24. Status: current. Part of [../README.md](../README.md).

Adds SpyServer's `FFT_IQ` streaming to our existing `spyserver_source` as
a **Mode** option.

* **Full IQ** stays the default and keeps today's behaviour.
* **VFO + server waterfall** streams a narrow IQ window plus the server's
  spectrum. The waterfall can then cover the device's full span at a
  fraction of the bandwidth: 40 MB/s of Int16 at 10 MS/s against about
  190 kB/s.

## Sources of knowledge

* **jp's `spyserver_vfo_source`** (SDRPlusPlus-jp, `c48ba994`) gives the
  session order, the span/decimation rules and the gain servo; see
  [../../../research/forks/jp.md](../../../research/forks/jp.md) §4 and
  [../../../design/dsp-pipeline.md](../../../design/dsp-pipeline.md) §10.2–10.3.
  **Take its protocol knowledge, not its code.** Our client already has
  the body-size check and chunked publishing that jp's copy lacks.
* **The public directory** `airspy.com/directory/status.json` lists 218
  servers (RTL-SDR, Airspy HF+, Airspy One) with coordinates, client counts
  and `streamingHost`/`streamingPort`. Use it for Phase 0.

## Sub-plans

| plan | prerequisites | content |
|------|---------------|---------|
| [0-measurements.md](0-measurements.md) | none | protocol facts still unknown: retune marker, FFT scaling, span basis, slide limits |
| [1-client.md](1-client.md) | 0 | client: `CLIENT_SYNC`, FFT frames, write mutex, PING, IQ meter |
| [2-vfo-fft-mode.md](2-vfo-fft-mode.md) | 1, core 1 | mode, config, rate lists, session order, start/stop, `PrebufferedLink` |
| [3-waterfall.md](3-waterfall.md) | 2, core 3/4/5 | `SpyServerSpectrumProvider`: geometry tagging, stage zoom ladder, level calibration |
| [4-iq-window-and-gain.md](4-iq-window-and-gain.md) | 3, core 2 | window policy, commit rule from Phase 0, shared mode, gain servo |

**No TLS is needed:** SpyServer is raw TCP. The optional proxy CONNECT for
raw TCP is in transport/3.
