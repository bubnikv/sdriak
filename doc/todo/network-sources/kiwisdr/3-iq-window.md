# KiwiSDR 3: IQ window inside the server waterfall

Date: 2026-11-24. Status: implemented, not committed; the GUI is untested
and there is no live test of the commit yet. Where the code differs from
this plan:

* Window width is `audio_rate − 600` (300 Hz each side). The policy margin
  is 10 % of that.
* The slide limits keep the whole passband inside `0 … bandwidth`
  (baseband): `[rate/2 − C_bb, bandwidth − rate/2 − C_bb]`, mirrored under
  Invert IQ.
* `tune()` sends C + W at once. The UI tick sends a window move, at most 8
  per second. Any send that changes W arms the commit.
* The source counts the samples it pushes into the link
  (`sndInputBase + sndPushed`), so a packet's first-sample index is exact.
  The same index now records the seq playout times of kiwisdr/2.
* A marker fires before the whole output block holding its sample, so the
  mute is `2 × half + max(packet, 10 ms)`: ±6 ms on `NEW_FREQ`, ±20 ms
  (40 ms) without it. It goes through the new
  `VFOManager::muteAllFor()`.
* Without `NEW_FREQ` the commit lands on the first packet 150 ms after the
  command, with no RTT estimate. For KiwiSDR firmware that happens once
  400 ms pass with no flag.

Part of [README.md](README.md).
Prerequisites: KiwiSDR 2; core 2.

## Behaviour

* **The window.** With the server waterfall on, C is the waterfall centre
  and the 12 kHz (or 20.25 kHz) IQ stream is a window at C + W.
  * Window width = `sampleRate − 2·margin`, where margin = 300 Hz plus the
    passband filter's roll-off.
  * Passband: `±(sampleRate/2 − 100)`.
* **`tune(freq)`** only moves C. The FFT re-centres through the provider.
  The IQ window is placed by the policy.

## Steps (`kiwisdr_source`)

1. **UI tick** (`uiTickHandler`):
   1. Collect the VFO passbands (selected first). Call `chooseIQWindow()`
      with slide limits `[−C, bandwidth − C]` in offsets, so the window
      stays inside the receiver range after `freq_offset`.
   2. If the window changed: call `vfoManager.setIQWindow(W, width)`, then
      send `SET mod=iq low_cut=… high_cut=… freq=(C + W − tuningOffset − freq_offset)/1e3`.
      Keep it rate-limited (8 per second) and value-compared.
   3. Record `pendingW`.
2. **Commit on `NEW_FREQ`.** On the SND network thread, when a packet with
   `flags & 0x04` arrives while `pendingW` is set:
   * compute the switch index = the packet's first sample index +
     round(0.024 · sampleRate). The switch is 20–30 ms into the flagged
     packet; it was measured at 340 samples at 12 kS/s and 400–500 at
     20.25 kS/s.
   * `link.scheduleMarker(index, [W]{ vfoManager.commitIQWindowOffset(W); })`.
   * Mute every VFO for ±6 ms around the index with `muteFor()`, scheduled
     by a marker 6 ms earlier.
3. **No marker** (flavour `KiwiCompatible`, or no flagged packet within
   400 ms): commit by marker at the sample index of the first packet that
   arrives after `max(150 ms, 1.5 × RTT)` from the command. Use a mute of
   40 ms.
4. **Out-of-window VFOs** are marked and muted by core. KiwiSDR has one
   window per channel; a second channel costs a user slot and is not
   attempted.

## Review gate (user tests)

* **Drags:** dragging the selected VFO within the window gives no
  `SET mod` (hysteresis) and no glitch.
* **Window moves:** dragging past the edge moves the window with at most a
  short mute; the carrier stays at the same audio pitch before and after.
* **Second VFO:** a second radio VFO outside the window is drawn hatched
  and is silent.
* **Kiwi-compatible:** on `ea8bfk.ddns.net` (no `NEW_FREQ`) a retune
  still lands within the 40 ms mute.
