# UberSDR 4: IQ window and secondary VFOs

Date: 2026-11-24. Status: implemented and built, not committed; not run
against a server or in the GUI, and no unit test covers the routing (it
needs the GUI-side VFO globals). Part of [README.md](README.md).
Prerequisites: UberSDR 3; core 2.

Where the code differs from this plan:

* **Pending tunes:** every primary `tune` (centre retunes as well as window
  moves) is queued with its W, up to 16. An echo matching one commits that
  W and drops the older entries, so a burst of retunes commits in order.
  The mute runs only when W moves. A tune with no echo after 2 s commits at
  once, with 40 ms of mute (the server may clamp or drop a request).
* **Mute length:** 5 × max(packet, 360) samples from the first packet
  after the echo; the sizes of packets 2–5 are not known yet when it is
  scheduled.
* **Learning the offset:** not done; the mute covers the 3–5 packet cases.
* **Slide limits:** 10 kHz–30 MHz for the window's edges; the directory's
  `tuning_range` is not read.
* **Secondary commit:** on the echo, commit 7 samples before the end of the
  previous packet (the earlier candidate). Mute from 20 samples before that
  end through 20 samples past the end of the echo's packet.
* **Core API:** `IQFrontEnd::routeVFOInput()` as planned. `VFOManager` has
  `routeVFOInput(name, input, rate, offset, width)` on top of it, plus
  `setVFOWindow`, `commitVFOWindowOffset`, `muteVFOFor` and
  `isVFORouted`. The primary's commits and mutes pass routed VFOs by.
  A routed VFO bypasses the front end's pre-processing (decimation, DC
  block, IQ inversion), so the source conjugates a secondary's samples
  itself when the IQ is inverted.
* **Policy details:**
  * the primary window is placed over the VFOs that are *not* routed;
  * a VFO gets a secondary when its passband leaves the primary's intended
    window and is at most 90 % of the secondary's usable width (12 kHz ×
    0.9);
  * it returns to the primary once its passband is inside the primary
    window less a 10 % margin, or when it grows wider than the secondary;
  * a secondary is recentred on its VFO when the passband comes within
    10 % of its edge, at most 8 retunes per second;
  * a refused slot (`error`, or the socket closing) is not retried until
    the next session. The menu lists each VFO on its own stream and each
    refusal with the server's reason.

## Primary window

* **The window.** The primary IQ (`iq48`, or wider) is a window at C + W.
  Window width = 0.9 × sample rate. The ka9q-radio channel filter's exact
  edge is unmeasured, so use 0.9 until checked.
* **Placement.** In `uiTickHandler`:
  1. call `chooseIQWindow()`, with slide limits from the receiver range
     (`tuning_range` in the directory, or 10 kHz–30 MHz);
  2. `vfoManager.setIQWindow(W, width)`;
  3. `tune(C_hw + W, mode)`. No server-side limit on retune rate was seen
     (15 per second accepted), but cap at 8 per second.

## Commit rule (ubersdr.md §4, §4.1, §4.2)

* **What was measured.** After a `tune`, a `status` echo with the new
  frequency arrives in the IQ stream.
  * On the primary, the frequency switched **2 packets after the echo in
    17 of 20 retunes, and 3–5 packets after in the rest.**
  * The switch sits at a stable point in the server's block timing: near
    the end of a 240-sample packet (sample 224 or 233–235, fixed per
    server) or at the start of a 360-sample packet.
* **Implementation.** On the network thread, when the echo for the
  pending W arrives:
  * index = the first sample of the second packet after the echo +
    `packet_size − 7`;
  * `link.scheduleMarker(index, commit(W))`;
  * mute from the start of the first packet after the echo to the end of
    the fifth: `muteFor` scheduled by a marker.
* **Learning the offset (optional).** On the first retune, measure the
  in-packet offset with the carrier method from the research doc and
  shrink the mute.

## Secondary VFOs (`vfo=N`)

* **What the server offers.** A listener may hold up to `vfos − 1`
  secondaries: `/ws?…&vfo=N&mode=iq`, 12 kHz plain IQ, **never `iq48`+**.
  * They are not counted as users.
  * They live as long as the primary, plus 10 s of grace.
  * The secondary's echo arrives with the switch packet or one packet
    later, and the switch is at sample 233 of a 240-sample packet.
* **Core change: per-VFO external input.** This is optional and lives in
  this plan, because UberSDR is its first user.
  * Add `IQFrontEnd::routeVFOInput(name, dsp::stream<complex_t>* input, double sampleRate)`:
    unbind the VFO's `RxVFO` from `split` and point it at `input` with the
    given input rate.
  * `routeVFOInput(name, nullptr, 0)` restores it.
  * `VFOManager` keeps a per-VFO window offset for such VFOs, with the
    same `applyDspOffset` formula.
* **Source policy:**
  * a VFO that falls outside the primary window gets a secondary, while
    secondaries are available;
  * VFOs beyond the limit are marked out of window (core 2);
  * on close, route the VFO back.

  Each secondary has its own `PrebufferedLink` with the same target, so
  audio stays aligned.
* **Refusals** come as a JSON `error` after the upgrade, then a close:
  "This receiver allows N VFOs" or a mode error. Keep the VFO out of
  window and show the reason.

## Review gate (user tests)

* **Primary:** dragging within the window gives no retune, and a window
  move costs at most a short mute.
* **Secondary:** a second radio placed far from the first plays through a
  secondary stream.
* **Limit:** a third (on a `vfos: 2` receiver) is hatched, with the reason
  shown.
