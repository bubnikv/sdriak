# OpenWebRX+ 3: server waterfall and IQ window

Date: 2026-11-25. Status: implemented and built, not run in the GUI. Not
built: the calibration step. Rows are presented at the link's playout
time, with no latency constant added, as that latency is still unmeasured.
Part of [README.md](README.md).
Prerequisites: OpenWebRX+ 2; core-spectrum 1–5.

## Spectrum provider

* **Frames.** Each FFT row (step 1) becomes a `SpectrumFrame`:
  * `startHz = center_freq − samp_rate/2` plus the tuning offset, and
    `spanHz = samp_rate`, with Invert IQ handled as in `ubersdr_source`;
  * the bins as sent, already in display order;
  * dB as sent: log power with the server's fixed −70 dB added.
* **Zoom.** `zoomSpans()` is `{ samp_rate }`, a single step. Core's view
  zoom magnifies the frame, and that is all the zoom there is.
* **Default levels:** the profile's `waterfall_levels` from `config`
  (e.g. −88 to −20 dB), stored per provider as in core-spectrum/5.
* **Presentation.** Frames carry no timestamp. Present each frame at
  arrival plus the link's current prebuffer delay
  (`link.playoutTime(link.samplesIn())`), so it lines up with the audio to
  within the IQ path's extra latency.
  * That latency is unmeasured. Measure it once with the carrier method
    (a carrier keyed on and off would do), then add it as a constant.
* **Rows per second** vary by receiver (9 and about 50 measured). Feed
  every row: core's queue drops the surplus.
* **Unavailable:** no row for 3 s. Core then falls back to the local FFT,
  and the source does not register again in that session (as
  `kiwisdr_source` and `ubersdr_source` do).
* **Profile change:** a new span calls `core::setDisplaySpan()`, and the
  waterfall history is reset.

## IQ window

* **Width.** 36 kHz, from the measured passband with ±24 kHz cuts: flat to
  ±16 kHz, −1 to −2 dB at ±20 kHz, −5 to −7 dB at ±24 kHz.
* **Placement.** `chooseIQWindow()` over the VFOs, slide limits
  `±(samp_rate/2 − 18 kHz)`.
* **Moves** are `dspcontrol` `offset_freq = W` (in hardware Hz, with
  Invert IQ applied), at most 4 per second.
* **Commit by arrival**, as there is no marker (openwebrx.md §5):
  * the IQ moved in a message arriving 167–246 ms after the command, on a
    link whose round trip is included;
  * schedule the commit at the first IQ sample that arrives 200 ms after
    the send (a `scheduleMarker` on the link index of that sample);
  * mute every non-routed VFO from the first sample arriving 120 ms after
    the send to the last arriving 320 ms after;
  * superseding moves queue as in `ubersdr_source`: each commits its own W
    in order.
* **Calibration (optional, after the gate).** On the first move of a
  session, find the switch in the IQ by the change in short spectra (the
  method of `scripts/probes/openwebrx/owrxanalyze.py`). Shorten the delay
  and the mute to what was seen, plus a margin.
* **No secondary streams.** OpenWebRX+ allows one demodulator per
  connection. A second connection for a second VFO would count as another
  listener; this plan does not open one.

## Review gate (user tests)

* The waterfall shows the whole profile, and levels persist per provider.
* Dragging a VFO within 36 kHz gives no retune. Leaving it moves the
  window, with a mute of at most about 200 ms and no tuning error
  afterwards.
* With the IQ inverted, the waterfall and audio still agree.
