# SpyServer 4: IQ window, commit, shared mode, gain servo

Date: 2026-11-24. Status: implemented, not committed; GUI and live
untested. Where the code differs from this plan:

* **Device centre:** the slide limits use the last `CLIENT_SYNC`'s
  `DeviceCenterFrequency`, or the committed FFT centre before one arrives.
* **IQ sends:** every `IQ_FREQUENCY` (both modes, from `tune()` and the
  window) is clamped to the `CLIENT_SYNC` IQ limits and value-compared.
  That covers shared mode.
* **Stream index:** a `PONG` position is the client's published-sample
  count. `start()` records the link's index of the client's sample 0
  (`linkIndexBase`).
* **Mute:** ±3 ms around the `PONG`, plus one IQ message, since a marker
  fires before the whole block.
* **Rule C:** without a `PONG`, the commit lands when the 1 s timeout
  fires, with a 40 ms mute. That is later than the switch: there is no
  RTT estimate yet.
* **Servo switch:** one "Auto digital gain" checkbox per mode
  (`digitalGainAuto` for VFO+FFT, default on; `digitalGainAutoIq` for Full
  IQ, default off). The label shows the gain sent. With Auto off, the
  formula value is sent, as before. There is no manual slider and no
  `digitalGainManual` key.
* **Servo scope:** it runs only for UInt8 and Int16, and never on a
  headless server, which has no UI tick.
* **Retunes:** these snap the servo to the ceiling only while it runs.

Part of [README.md](README.md).
Prerequisites: SpyServer 3; core 2.

## IQ window

* **Width.** The window is the narrow IQ rate × (`MaximumBandwidth` /
  `MaximumSampleRate`).
* **Slide limits.** Device centre ± (`MaximumBandwidth` − window width)/2:
  Phase 0 found aliases, and no warning from the server, once the IQ passes
  the device's alias-free band. That is narrower than jp's ±(`MaximumSampleRate`
  − IQ rate)/2. In FFT+IQ mode the device centre is the FFT centre, so a
  window beyond the limit needs an FFT move first. Intersect with
  `CLIENT_SYNC`'s min/max, which on a capped server are exactly this range.
* **UI tick:**
  1. `chooseIQWindow()`;
  2. `vfoManager.setIQWindow(W, width)`;
  3. send `IQ_FREQUENCY = C_hw ± W`, where the sign flips with Invert IQ.
     Rate-limit to 8 per second and compare values.
* **Commit: rule B, then C** (Phase 0).
  * On the `PONG` of the setting's `PING`:
    `link.scheduleMarker(streamPosOfPong, commit(W))`, with a mute of a few
    milliseconds plus one output block (the switch lies within about 20
    samples of the `PONG`).
  * With no `PONG` after about 1 s (rule C): commit at the first packet
    after `max(150 ms, 1.5 × RTT)`, with a 40 ms mute. The switch came
    265–560 ms after the command in Phase 0.
* **One IQ window per connection.** VFOs outside it are marked and muted
  by core.

## Shared mode (`canControl == 0`)

Another client fixes the device centre. Clamp `tune()` and the window to
the `CLIENT_SYNC` limits, and show "Shared: device centre fixed by another
client".

* A second client takes control away from the first as well, so this mode
  can start mid-session.
* The server ignores out-of-range requests without a reply, so clamp
  before sending.
* The guest's IQ limits are its own FFT window (FFT centre ± span/2).

## Digital gain servo (jp's; credit it)

* **Constants:**

  | constant | value |
  |----------|-------|
  | target peak | −6 dBFS |
  | α when cutting gain | 0.80 |
  | α when raising gain | 0.30 |
  | rail threshold | 0.1 % of samples |
  | step on hard clip | 3–24 dB, scaled by the railed fraction (full at 20 %) |
  | minimum interval between adjustments | 150 ms |

  The gain is clamped to [0, formula ceiling] and sent only when the
  integer value changes.
* **Where it runs:** in the UI tick, not on a thread. The meter comes from
  SpyServer 1.
* **Both modes.** The default is on for `vfo_fft` and off for `iq`. With
  it off, Full IQ sends exactly today's formula value.
* **`applyDigitalGain(reset)`** is the single place that sends digital
  gain:
  * `reset = true` (start, retune, Auto re-enabled) snaps to the ceiling;
  * `reset = false` (gain slider, format or rate change) only moves the
    ceiling.
* **HF+ UInt8 baseline.** `computeDigitalGain()` adds jp's 32 dB HF+ UInt8
  baseline **only while the servo is on**.

## Review gate (user tests)

* **Window:** drags inside the window send no `IQ_FREQUENCY`; a window
  move causes at most a short mute.
* **Second VFO:** a second VFO outside the window is hatched and silent.
* **Shared mode:** with a second client (SDR#) holding control, the limits
  are respected.
* **Servo:** a strong signal on UInt8 does not clip with the servo on;
  Full IQ with the servo off is identical to today.
