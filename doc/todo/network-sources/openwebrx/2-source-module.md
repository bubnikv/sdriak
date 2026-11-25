# OpenWebRX+ 2: source module

Date: 2026-11-25. Status: implemented and built, not run in the GUI. Part
of [README.md](README.md). Where the code differs from this plan:

* **With the server waterfall off** (or after core fell back), the display
  is the IQ itself, centred at C + W, and tuning moves W within the
  profile. W = 0 would leave only the 48 kHz around C to listen to.
  * A retune mutes at once, for the prebuffer plus the IQ arriving until
    the switch: the display has already moved.
  * A tune outside the profile is clamped to its edge, with a notice.
* **The wait for `modes`** is 10 s. Without a list, a supported version is
  tried anyway.
Prerequisite: OpenWebRX+ 1.

## Files

* `source_modules/openwebrx_source/CMakeLists.txt`, with
  `include(${SDRPP_MODULE_CMAKE})`.
* `source_modules/openwebrx_source/src/main.cpp`.
* Root `CMakeLists.txt`: `OPT_BUILD_OPENWEBRX_SOURCE` (default ON) and the
  module directory.
* `core/src/core.cpp`: the default module list and the Android list.

## Module

* **Registration.** `registerSource("OpenWebRX+", &handler)` with menu,
  select, deselect, start, stop, tune and `uiTickHandler`. One instance.
  Not served in `--server` mode, as `ubersdr_source`.
* **Menu:**
  * server URL, with a recent list of 8;
  * status line, receiver name and location, session time left;
  * the profile combo;
  * buffer (ms);
  * a "Server waterfall" checkbox, used in step 3.
* **Start:**
  * connect and wait for the handshake and the first complete `config`;
  * **refuse** when the version is below 1.2.129, or `iq` is missing from
    `modes`: "needs OpenWebRX+ 1.2.129 or later (this receiver runs
    <version>)";
  * otherwise start IQ at W = 0 and set the input rate to 48 000.
* **Stop** closes the socket.

## Fixed centre

* **C is the profile's `center_freq`.** The receiver tunes within the
  profile, not the hardware. Follow `file_source`'s handling of a source
  whose centre the user cannot move:
  * the waterfall centre comes from the source;
  * the frequency selector is constrained to `center_freq ± samp_rate/2`
    (`SourceManager`'s constraint API).
* **`tune()`** keeps C. A tune request outside the profile span is
  dropped, and the status line says so.
* **Before step 3** the IQ window is fixed at W = 0. VFOs outside it are
  hatched by core-spectrum/2.

## Profiles

* **A profile change retunes the shared SDR for every listener on it.**
  The combo asks for confirmation every time, stating that, and naming
  the listener count from `clients`.
* **Rate limit:** at most one change per 15 s. The server bans a client
  for 12 hours when its score reaches 30, and the score grows by
  `10 − Δt` for each change less than 10 s after the previous one.
* **Changes made by others** arrive as a `config` with a new
  `center_freq`/`samp_rate` (step 1):
  * apply the new C and span on the UI thread;
  * clamp the VFOs (`MainWindow::clampVFOsToSpan`);
  * restart IQ at W = 0;
  * say so in the status line.

## Audio path

* **Link.** IQ goes into a `PrebufferedLink` (150 ms default, like the
  other network sources) at 48 kS/s.
* **Rate check.** Measure the arrival rate against 48 000 over 5 s
  windows. Below 98 % twice, show "link too slow": there is no narrower
  IQ to fall back to.
* **ADPCM.** When `audio_compression` is "adpcm", the menu says the IQ is
  4-bit compressed (lossy) on this receiver.

## Review gate (user tests)

* SSB and AM on an HF profile play through our demodulators at W = 0.
* An upstream receiver is refused with the version message.
* A profile change asks first, is refused within 15 s of the previous one,
  and moves the display when it happens.
