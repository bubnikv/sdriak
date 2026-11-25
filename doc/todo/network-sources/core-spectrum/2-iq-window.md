# Core 2: IQ window, stream-position commit, mute

Date: 2026-11-24. Status: implemented, not committed; no caller yet.
`core/tests/iq_window_tests.cpp` covers the policy, markers in bypass and
prebuffer mode, and the mute; it passes in the bench and ASan trees. Where
the code differs from this plan:

* `spyserver_source` gains its `PrebufferedLink` in spyserver/2, which
  already plans it, not here.
* An out-of-window mark also mutes the VFO (`RxVFO::setMuted`).
* The counter and markers live in `dsp::buffer::StreamPosition`, shared by
  the `Prebuffer` and the bypass `PositionLink`.
* The policy keeps the window while every passband fits the full width;
  the margin applies only when it moves.

Part of [README.md](README.md).

## Problem

A networked source in server-waterfall mode streams a narrow IQ window
centred at C + W, while the VFOs are placed anywhere in the displayed
span. Three things follow:

* each VFO's DSP mixer must be set to `centerOffset − W`;
* VFOs outside the window must be marked and muted, not silently
  mistuned;
* when W changes, the DSP offsets must change **at the sample where the
  server's IQ actually switched**. A 100–250 ms prebuffer sits between the
  network thread and the VFOs, so committing when the network thread sees
  the server's marker would be 100–250 ms early.

## Adds

### 1. `VFOManager` (`core/src/signal_path/vfo_manager.{h,cpp}`)

* **One function sets every DSP offset.** Add private
  `applyDspOffset(VFO*)`:
  `dspVFO->setOffset(vfo->m_dspCentre.load() − m_windowOffset.load())`.
  Route the three existing writes through it: `VFO::setOffset`,
  `VFO::setCenterOffset` and `updateFromWaterfall`. Grep the tree for
  `dspVFO->setOffset` and convert any other caller.
* **Atomic centre per VFO.** `VFO::m_dspCentre` is a `std::atomic<double>`
  written wherever `wtfVFO->centerOffset` is set. The marker callback
  (below) reads it off the UI thread instead of racing on `wtfVFO`.
* **Map lock.** `m_vfoMtx` guards the VFO map in `createVFO`, `deleteVFO`
  and the off-thread commit.
* **Window API:**

  ```cpp
  // UI thread: the window a source intends; updates out-of-window marks.
  void setIQWindow(double offset, double width);
  void clearIQWindow();                       // W = 0, width unlimited
  // Any thread (marker callback): the offset the IQ samples now have.
  void commitIQWindowOffset(double offset);   // atomic store + applyDspOffset on all VFOs under m_vfoMtx
  double committedIQWindowOffset() const;
  bool isOutOfWindow(const std::string& name) const;
  ```

  Keep **intended** and **committed** offsets separate. Out-of-window marks
  use the intended window; DSP offsets use the committed one.
* **Out of window** means the passband `[lowerOffset, upperOffset]` is
  not inside `[W − width/2, W + width/2]`. Recompute on the UI thread in
  `setIQWindow` and `updateFromWaterfall`, and wherever a VFO's
  bandwidth or reference changes.

### 2. Mute and mark

* **RxVFO.** `core/src/dsp/channel/rx_vfo.h`: add to `RxVFOKernel`
  * `std::atomic<bool> m_muted`, which makes `process()` write zeros but
    still run the resampler, so the sample count is kept;
  * `std::atomic<int> m_muteSamples`, a countdown in input samples used to
    hide a commit transient.

  `RxVFO` exposes `setMuted(bool)` and `muteFor(int inputSamples)`.
* **Waterfall.** `core/src/gui/widgets/waterfall.{h,cpp}`: add
  `WaterfallVFO::outOfWindow`, drawn hatched or dimmed, with the tooltip
  "Outside the server's IQ window".

### 3. Stream-position markers

In `core/src/dsp/buffer/prebuffer.h`, `PrebufferedLink<T>`:

```cpp
uint64_t samplesIn() const;     // total input samples accepted so far
// Fire cb on the link's output thread right before the output block that
// contains input sample `inputIndex` is published. Works in prebuffer and
// bypass mode; survives the prebuffer's drop-oldest trimming (a marker
// whose sample was dropped fires with the next published block).
void scheduleMarker(uint64_t inputIndex, std::function<void()> cb);
// Expected steady_clock time at which input sample `inputIndex` leaves the
// link (core 3 uses it for frame presentation).
std::chrono::steady_clock::time_point playoutTime(uint64_t inputIndex) const;
size_t bufferedSamples() const;
```

* **Index bookkeeping.** Counting is in input samples. `Prebuffer` tracks
  the input index of its ring head, advanced by pops and by
  `dropOldestLocked`. In bypass mode the `StreamLink` path counts the
  samples it forwards.
* **Error budget.** After the link come `inBuf`, `split` and the `RxVFO`
  block, so a commit can land up to about one block early. The source
  pairs every commit with `muteFor()` covering that block plus the
  server's own uncertainty (per source: KiwiSDR ±5 ms, UberSDR 2–5
  packets, SpyServer from its Phase 0).
* **Network sources use the link.** `kiwisdr_source` and the new
  `ubersdr_source` use `PrebufferedLink`. `spyserver_source` gains one
  (default target 0 ms = bypass) so that markers work.

### 4. Shared window policy

`core/src/signal_path/iq_window_policy.h`, header-only and pure, so it can
be desk-checked:

```cpp
struct IQWindowInput {
    std::vector<std::pair<double,double>> passbands;  // [lo, hi] offsets from C, selected VFO first
    double currentOffset, width, margin;              // margin ≈ 10 % of width
    double minOffset, maxOffset;                      // server slide limits around C
};
// Hysteresis: keep currentOffset while every passband fits; else centre on
// the union if it fits, else on the selected VFO; clamp to limits.
double chooseIQWindow(const IQWindowInput& in);
```

## Review gate

* With no window set, `W ≡ 0`, and `applyDspOffset` gives the same values
  as today.
* No VFO is ever marked or muted.
* The marker API is unused but compiled.
