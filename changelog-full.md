# Complete changelog

The detailed per-release history of the SDRIAK fork, including alpha and beta pre-releases. For a brief summary of the major releases only, see [changelog.md](changelog.md).

## v1.4.0-beta2 - 2026-09-08

This follow-up beta completes the product rename from SDR++ iak to SDRIAK, makes QMX VFO synchronization behave like a centered panadapter, and resolves several touch-input and application-icon integration issues.

**Upgrade notes:** Desktop executable, installation and default configuration paths now use `sdriak`; settings in the previous beta's `sdrpp-iak` config directory are not migrated automatically. Android retains the existing `org.ok1iak.sdrpp` application ID and therefore upgrades in place with its app data intact. The SDRIAK Server protocol magic, fork ID and authentication salt also changed, so beta and beta2 clients and servers must be upgraded together; previously saved server passwords must be entered again.

### Added

- QMX **Sync VFO** now provides centered-panadapter behavior: the selected SDRIAK VFO is held at the QMX IF/CW offset from the IQ center and remains at the center of the waterfall independently of zoom. Frequency changes initiated by either QMX or SDRIAK update the other side without feedback loops.
- Bidirectional QMX mode synchronization. Current QMX firmware's AM mode maps to SDRIAK AM in both directions, and CW and CW-R follow the sideband implied by the QMX CW offset.
- Touch-friendly visible handles for the menu and FFT/waterfall splitters are now available whenever Touch-Friendly UI is enabled, including on touch-screen desktop systems.
- Added the Wave AM application-icon pack, with authored assets for Android launcher densities, Linux hicolor PNG/SVG installation, macOS `.icns`, Windows multi-resolution `.ico`, and in-app loading, Credits and toolbar presentation.

### Changed

- Renamed the product from **SDR++ iak** to **SDRIAK** throughout user-visible text, modules, documentation, build options, executable/library names, packages, desktop integration and CI artifacts. Legacy `sdrpp` identifiers remain only where compatibility requires them.
- Renamed the network source and module to **SDRIAK Server**, and changed the fork-specific protocol identity and authentication salt from their `sdrpp-iak` values to `sdriak` values.
- Desktop default config roots, AppImage roots, install/resource/plugin directories, executable names and package names now use `sdriak`. The macOS bundle is now `SDRIAK.app`; the Android namespace and native library names were updated while its published application ID was deliberately retained.
- Waterfall frequency-scale drag, wheel and arrow-key panning now share one tuning path. When the QMX VFO is locked to the IQ center, these interactions tune the radio while preserving the view bandwidth and VFO position.
- Android splitter recognition continues to delay a grab until the initial drag direction is known outside the visible handle, allowing vertical menu scrolling near a divider; mouse users retain an immediate precise divider target.
- The Credits dialog now uses the standard scrollbar width on every platform instead of the finger-sized bar previously drawn on Android. Its body drag-scrolls anywhere, so the wide bar only consumed content width.
- The top-bar level meter is capped at a useful draw width on large displays while remaining right-aligned.
- Expanded the fork research documentation with the M0OPK panadapter fork and updated the QMX-panadapter comparison and synchronization documentation.
- Refreshed SDRplay API download URLs. Debian 11 Bullseye `.deb` jobs were disabled after Bullseye's LTS end of life and removal of its security packages; the Ubuntu Focal AppImage retains the same glibc 2.31 baseline for Bullseye users.

### Fixed

- Selecting a different VFO on the waterfall is now a selection-only gesture: the initial mouse-down or finger-down changes selection, suppresses waterfall and overlay interaction for the remainder of that press, and cannot immediately drag or retune the VFO. Interaction resumes only after release, including when release occurs outside the waterfall. Fixes [#21](https://github.com/bubnikv/sdriak/issues/21).
- Android now reports application focus changes to ImGui, releases emulated mouse buttons and resets the touch-scroll and pinch recognizers when focus or the native window is lost. Android motion `ACTION_CANCEL` also releases tracked buttons, preventing interrupted gestures from leaving waterfall interaction latched after switching applications.
- Android Back reliably dismisses the Credits modal, and the dialog's popup lifecycle resets correctly whether it closes by Back, Escape or a tap.
- Completed application-icon integration across runtime window icons, task switching, executable/bundle metadata and desktop installation, including restoring window icons on BSD GLFW builds and keeping platform-only assets out of runtime resource bundles.
- Universal macOS bundle generation now removes duplicate runtime search paths before signing.

## v1.4.0-beta

This beta turns the alpha's experimental band picker and waterfall autoscale into more complete, stable features. It also adds a native macOS audio sink and spectrum-range navigation, makes network connection handling bounded and cancellable, and substantially hardens configuration persistence, server operation and fractional-scale rendering.

**Alpha testers:** delete the fork's config directory before upgrading to this beta. Pre-release numeric radio modes and frequency-memory layouts are deliberately not migrated.

### Added

- **Bands / Frequency — Spectrum page:** jump between named radio-spectrum ranges, clipped to the active source's tuning limits, while remembering the last frequency used in each range. The dialog's page selector and the Bands-page category selector now use a shared touch-friendly segmented control.
- Native macOS CoreAudio output sink, enabled by default alongside PortAudio. It uses UTF-8 device names, follows the selected device's nominal sample rate, avoids allocation in the render callback and falls back to a null drain when setup fails so the FFT keeps running. Addresses [AlexandreRouma/SDRPlusPlus#1776](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1776).
- Continuous waterfall auto-range: hold the contrast button to latch tracking, or tap for a one-shot fit. The estimator uses robust quantiles over the visible raw FFT span, excludes edges and DC, pools recent lines for one-shot fits, and applies asymmetric smoothing and a deadband in continuous mode.
- Shared cancellable asynchronous connector and timed DNS/connect infrastructure, initially used by the SDRIAK Server and SpyServer sources; all remaining blocking TCP connection paths now have finite timeouts. The original work was contributed by edudant in [PR #20](https://github.com/bubnikv/sdriak/pull/20), resolving [issue #19](https://github.com/bubnikv/sdriak/issues/19).

### Changed

- Waterfall level controls are now **Ref + Range** instead of independent Min/Max sliders. Auto-range moves Ref while preserving the chosen contrast range; its latched state is visible and persisted, and the control strip adapts to short touch displays.
- Band selection and IC-705-style three-register band stacking were stabilized around canonical band identities. Legacy plan segments are grouped by stable ID, overlap/service resolution is deterministic, repeat selection rotates all three fixed register positions, and the mapping provenance can be audited with the new developer tooling.
- The Bands / Frequency dialog now sizes itself from the viewport, varies grid columns and rows with available space, recentres after rotation/resize, keeps its readout and controls stable as messages change, and avoids nested scrolling on small landscape screens.
- Configuration access across core and all modules now uses scoped read/edit transactions. Unchanged values no longer dirty and rewrite `config.json`; malformed values fall back safely; lock lifetimes are shorter; shutdown flushes pending saves; desktop instances merge concurrent edits; and focused persistence/Windows-lock tests were added.
- Relative module/resource/config paths are resolved from the executable rather than the process working directory. Windows builds also declare UTF-8 as the active code page and initialize console output accordingly. Prompted by [AlexandreRouma/SDRPlusPlus#1265](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1265).
- Radio-mode names now come from one compile-time-checked table across the UI, demodulators, recorder and rigctl. Band-stack registers, bookmarks and the selected demodulator persist those stable names, removing enum-order dependencies from the first public beta's on-disk formats. Explicit bookmark import still accepts upstream SDR++'s numeric modes and normalizes them to names; export remains this-fork format. Rigctl capability reporting now includes CWR.
- Fractional UI scales render more crisply: fonts, waterfall/FFT geometry, toolbar icons, meter bars, menu strokes and frequency digits use consistent whole-pixel metrics. Added the 125% and 175% presets requested in [AlexandreRouma/SDRPlusPlus#1116](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1116) and [PR #1115](https://github.com/AlexandreRouma/SDRPlusPlus/pull/1115); ineffective sub-100% choices are disabled on standard-DPI displays.
- Developer documentation was reorganized into design, research, bug and to-do sections and expanded with band mapping/stacking, radio-mode, UI-thread, configuration and community-fork reviews.

### Fixed

- Android radiosonde decoder abort while handling RS41 frames: the vendored `sondedump` decoder treated an eight-byte, non-terminated serial field as a 31-byte C string copy, which Android's fortified libc rejected as a source-buffer over-read. The dependency patch now copies exactly the packed field width and retains the existing explicit terminator. Thanks to [@jprincl](https://github.com/jprincl).
- SpyServer and SDRIAK Server connections no longer leave the UI hanging during unreachable DNS/hosts; connection attempts can be cancelled, failures are reported in the source panel, failed Winsock non-blocking connects are detected correctly, and dead heartbeat-capable server links are noticed. Addresses [AlexandreRouma/SDRPlusPlus#1462](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1462).
- Socket descriptors are closed on networking error and remote-disconnect paths, preventing the rigctl server from eventually exhausting descriptors and refusing restarts. Listener shutdown and connection waiting races were fixed as well. Fixes [AlexandreRouma/SDRPlusPlus#1061](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1061).
- Rigctl/Hamlib interoperability: accept VFOA/VFOB/VFOC/current/None tokens, provide receive-only PTT replies, handle absent VFOs/demodulators safely, clear partial commands between clients and keep advertised modes consistent with accepted modes. Addresses [AlexandreRouma/SDRPlusPlus#1506](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1506) and [#1092](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1092).
- Server-mode source menus no longer dereference a missing ImGui context when drawing disabled controls (notably SDRplay RSP2 Hi-Z, SpyServer, Spectran HTTP and QMX Server). Fixes [AlexandreRouma/SDRPlusPlus#1630](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1630).
- Scanner restart/destruction no longer calls `std::terminate` after its worker exits by itself; worker state and ImGui disabled scopes are also race-safe. A concurrent radio-mode update no longer unbalances the ImGui stack. Fixes [AlexandreRouma/SDRPlusPlus#1658](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1658) and [#1437](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1437).
- Fixed the menu iterator invalidation crash when a draw handler creates a module ([AlexandreRouma/SDRPlusPlus#816](https://github.com/AlexandreRouma/SDRPlusPlus/issues/816)), and closed partially opened RtAudio streams before switching to null-drain fallback.
- DSP correctness fixes: guard SNR calculation when no out-of-band bins exist, avoid a negative shift while configuring an upsampling rational resampler, and publish the frequency translator phase increment without a data race or narrowing conversion.
- Config loading no longer deadlocks on early returns; BladeRF settings are not read after unlocking; LimeSDR's LNAW fallback compares antenna names correctly; shutdown saves retry only genuinely transient Windows replacement interference and fail promptly for read-only/structurally invalid destinations.
- A malformed user band-plan file no longer aborts startup: the file is named in the log and skipped while the remaining plans load. Unknown `def_mode` strings are also reported before falling back to the runtime convention.
- Assorted layout/build fixes: stable VFO-change reporting, valid positive band spans, Windows `min`/`max` and `ERROR` macro collisions, CMake runtime-DLL policy warnings, and stale waterfall lines/locks/buffers in auto-range processing.

## v1.4.0-alpha - 2026-07-19

A large UI-focused release. The headline is a touch-friendly interface overhaul that makes SDRIAK usable with a finger on Android (and previewable on desktop): an Android-metrics style overlay, a Material 3 dark theme, single-finger drag-scroll with fling, pill-handle window splitters, and an IC-705-style direct frequency-entry keypad with an integrated band picker. A new shared `PopupDialog` widget brings consistent keyboard and back-gesture handling to every modal in the app. Alongside these, a batch of desktop input fixes, radio/DSP touches, and crash fixes ported from and reported against upstream SDR++.

The band stacking feature inspired by ICOM radios is work in progress, not working correctly yet.

The waterfall autoscale is work in progress: Both the algorithm and user interface will change.

### Added

#### Touch-friendly UI

- Touch-friendly UI style overlay (`style::touchStyle`): an Android-like sizing layer applied as the final step of every re-style path (startup, scale change, Android re-init, theme switch), in dp units — ~48 dp row pitch, 24 dp slider grabs, touch extra padding, thin rounded scrollbars, Material-like rounded borderless frames and popups. It touches sizes only, so it composes with every theme. On by default on Android, off elsewhere, and toggleable live via a "Touch-Friendly UI" checkbox in the Display menu for desktop preview. The overlay was later toned down to at most ~25% over desktop metrics so it stays compact on desktop.
- Android Dark theme: a Material 3 dark palette for the touch UI — the baseline neutral surface ramp (`#141218` → `#36343B`), a blue tone-80 accent (`#A8C7FA`), precomputed 8%/12% state-layer blends for hovered/pressed, opaque popups and transparent borders. Instrument colors (FFT hold, waterfall background, meter histogram) are kept functional.
- Android: single-finger drag-scroll with fling for the menu and other scrollable windows. A recognizer sits ahead of the pinch recognizer in the input chain and withholds the touch-down from ImGui until the gesture is classified, so scrolling can never activate a widget under the finger: a vertical drag past 8 dp slop pans the window 1:1 (`SetScrollY`) and flings on release with hardware-timestamped velocity, a horizontal drag replays the press so sliders and the splitter keep working, a motionless ~200 ms hold replays the press (menu drag-reorder becomes press-hold-drag), and a quick tap replays press+release. Touches over the waterfall, top bar, scrollbars or vertical-slider column pass straight through with no added latency.
- Android: touch-friendly window splitters with visible pill-shaped drag handles and haptic feedback (routed through a new `backend::hapticTick()` JNI call), replacing the invisible fat hit bands that stole touches from the menu scrollbar, the frequency scale and the top of the waterfall.
- Radio: compact mode selector rendered as a wrapping toggle-button grid (a reusable `doToggleButtonGrid` widget, PowerSDR/Thetis-style button bank) instead of the 3-row `Columns(4)` radio buttons — rows auto-wrap and balance to the panel width, and display order is decoupled from `DemodID` (NFM WFM AM DSB RAW / LSB USB CW CW-R). The active mode is highlighted in the grid.

#### Bands / Frequency dialog

- **Frequency page:** an IC-705-inspired direct frequency-entry keypad. Holding any frequency digit for 0.5 s (mouse or touch, with a haptic tick on Android) opens the dialog — entry is in MHz with a single decimal point, ENT zero-fills the digits below the last entered one, CE clears, and pressing `.` first re-enters the current MHz digits for quick in-band retunes. Hardware keyboard (digits, dot, Backspace, Enter, Escape) works alongside the buttons and committed values are clamped to the tunable range. The layout is a 4×4 grid with a Backspace key in the Android PIN-pad position and a double-height ENT. A dialpad button next to the frequency display opens the same dialog, shown only when the top bar has room without squeezing the volume slider.
- **Bands page:** a category-filtered band grid in the Bands / Frequency dialog, built from the selected band plan. Legacy plan segments carrying the same stable `band_id` collapse into one canonical key while their original ranges remain available for containment and overlap resolution. Each band owns three rotating optional frequency/mode entries; the top entry is always current, so no separate pointer is persisted. Tapping the active band stores the current frequency, rotates all three fixed positions (third to top, top to second, second to third), initializes an empty new top from the just-saved current state, and recalls it. Long-pressing the active band keeps the live VFO state visible at the top without overwriting the saved entry if the list is cancelled; only an empty top is initialized, from an in-band VFO or the band's usable default. Choosing a populated row promotes it; an empty row can first capture the live VFO state when it still belongs to the band and is otherwise disabled. Opening the Band page resolves and scrolls to a matching band only inside the restored group without writing a register, while lifecycle save may follow manual tuning to another band only inside the current service. A band without memory uses the band plan's default frequency/mode, else a valid midpoint within the band's segment union with a heuristic mode convention. Page and category selections persist under `frequencyMemory`.
- Band-plan tuning defaults: `scripts/enrich_bandplans.py` merges default tuning info (default frequency, channel spacing, monitoring mode) from the KiwiSDR band database (`dist.dx_config.json`, [jks-prv/KiwiSDR](https://github.com/jks-prv/KiwiSDR), GPLv3) into the band-plan JSON files, matching by service category and frequency overlap with ITU-region preference. Only values genuinely present in the source are written; a mode that merely repeats the band-type convention is filtered out. The script edits files surgically (preserving indentation and line endings byte for byte), is idempotent, and the loader ignores unknown keys so the files stay backward compatible.

#### Waterfall

- Waterfall FFT min/max auto-range button ("A", under the Min/Max sliders): sets the low/high thresholds from the current FFT, scanning the middle 60% of the row to avoid band edges, filter roll-off and the DC/center spike, and persists the result. Ported from ericek111's fork, addressing AlexandreRouma/SDRPlusPlus#1729. Thanks to @ericek111 (Erik Brocko).
- Waterfall colormaps from AlexandreRouma/SDRPlusPlus#1694. Thanks to @konung-yaropolk.

#### Other

- SDRplay: per-device PPM frequency correction with persistence and live updates, adding `InputFloat`/`InputDouble` support to `SmGui` for the server-mode UI along the way. Addresses AlexandreRouma/SDRPlusPlus#1781. Thanks to @M0OPK.
- Frequency manager: right-click on the FFT or waterfall creates a bookmark at the frequency under the cursor, seeded with the selected VFO's bandwidth and mode. Implemented entirely in the frequency_manager module via the existing input handler. Ports the idea from upstream PR #1476 (issue #1475). Thanks to @Zaryob.
- Radio: CW pitch/offset preset value controls, with a step-and-popup value editor aligned to the other preset controls.
- Theme selector moved into the Display panel (refs AlexandreRouma/SDRPlusPlus#1008).

### Changed

- `PopupDialog` widget: a new core widget that encapsulates the two idioms every modal-ish popup in the app used to hand-roll — a lifecycle that arms `OpenPopup` exactly once per request and detects an ImGui-level close (Android back gesture, click outside) so it can't be undone by an unconditional re-open next frame, and focus-scoped, edge-triggered Enter/keypad-Enter to apply and Escape to cancel. It was rolled out across the app:
  - `GenericDialog` rebuilt on top of it, fixing all seven call sites (frequency-manager delete confirms, source-offset delete, module-manager confirm/error, server-source busy/auth, iq_exporter error), which previously could not be dismissed by back gesture / click-outside and had no keyboard support.
  - Frequency manager dialogs (bookmark add/edit, new/rename list, select-lists) ported over, gaining consistent Enter=apply / Escape=cancel and back/click-outside dismissal, with paired open flags collapsed into mode enums so "both dialogs open at once" is unrepresentable. Saving a bookmark with Enter/keypad-Enter and dismissing with Escape fixes AlexandreRouma/SDRPlusPlus#1758.
  - Source-menu add-offset dialog and the scheduler's task-edit popup adopted the same lifecycle.
- Android navigation reworked around the platform task model. One Back press now dismisses one UI layer (topmost popup/modal, then credits, then — earlier in development — the menu), unwinding nested popups per press; Back arrives through both dispatch models (`KEYCODE_BACK` on Android ≤ 15 and `OnBackInvokedCallback` on Android 16+ with targetSdk 36). In the final design Back no longer collapses the menu panel or offers to exit — the dismiss chain ends by backgrounding the app via `moveTaskToBack`, matching the Android 12+ default — and **exiting is done by holding the hamburger menu button for 0.6 s** (haptic tick, then an Exit confirmation modal that stops the SDR and finishes the task). The old "Back Button Asks to Exit" System option was removed with this change.
- Desktop widgets gained subtle rounding (frames/grabs/tabs 3 dp, popups 5 dp, scrollbar grabs become pills); windows and children stay square since they run edge-to-edge. The touch overlay's rounding is separate.
- Left-menu layout hardened against the touch style: width math in several panels (FolderSelect/FileSelect "…" row, frequency-manager list selector, sink volume slider, Spots enable column, module-manager buttons, WebSDR view) now computes from live `FramePadding`/`ItemSpacing`/`GetFrameHeight()` instead of hard-coded 1× pixel constants. Full-width action buttons (Record, Start/Stop, Apply, …) are inset via new `ImGui::ActionButton()`/`BeginActionRow()` helpers (16 dp touch / 10 dp desktop per side) so `CollapsingHeader` bars remain the only edge-to-edge elements. The menu-header enable checkbox alignment is likewise derived from style metrics.
- The `doFingerButton` 3×-font-height touch-button wrapper was removed now that the touch style overlay owns touch sizing globally; call sites use plain `ImGui::Button` and measure with `GetFrameHeight()`.
- RDS PS Name, RadioText and PTY Name now decode the RDS/EBU G0 character repertoire to UTF-8 at read time (raw storage unchanged), and the repertoire glyphs Roboto-Medium provides are baked into the ImGui atlas so non-ASCII station text renders correctly. Based on the approach in AlexandreRouma/SDRPlusPlus#1164, reworked to translate at read time rather than switching storage to `std::wstring`. Thanks to @attah.
- Debug menu (and the ImGui demo window) now compile only into development builds — any build with commits past the last `vX.Y.Z` release tag, or a Debug configuration, or a CI nightly (any GitHub Actions build that isn't a release-tag build). Release-tag artifacts ship without the menu; `IMGUI_DISABLE_DEMO_WINDOWS` reduces `imgui_demo.cpp` to stubs while keeping exported symbols.
- Module-com contract headers (`radio_interface.h`, `meteor_demodulator_interface.h`, `recorder_interface.h`) moved into `core/src`. These interface-only enum headers were reached via cross-module include paths by seven modules — a layering violation once core itself became a consumer (the band picker) — and all 11 cross-module `target_include_directories` entries are now gone: modules depend only on core at compile time.
- Android: the app now uses the SDRIAK launcher icon; the inert `proguardFiles` line was removed from the release build (R8/ProGuard never ran, so the app ships unobfuscated, as expected for an open-source project).
- CW filter defaults adjusted; the two `showHzPresetInput` int/float overloads were merged into a single `if constexpr` template; the Spots "Spot Lifetime" label now states its units (minutes).
- Internal: `SmGui` `DrawListElem` scalar payloads folded into an anonymous union (one scalar is live per element), shrinking each element with no wire-format or field-access change.

### Fixed

- Startup crash on a machine with **no audio output device**: RtAudio enumeration left the device list empty, so the audio sink indexed past its end (ACCESS_VIOLATION before the GUI appeared). `selectFirst()`/`selectById()` are now bounds-guarded and the menu shows "No audio output devices". Guarding alone would have stalled the whole DSP pipeline (the undrained sink blocks the shared IQ front-end that also feeds the FFT), so when no real device can be opened the sink now falls back to a lazily-constructed `dsp::sink::Null`, keeping the spectrum/waterfall live headless. Reported as #1754.
- Crash on window resize caused by an unsynchronized resize of the FFT buffers.
- Waterfall FFT auto-range ("A") no longer persists a bogus range when clicked with no live source: before the first frame `latestFFT` held the −1000 "hide everything" sentinel (or uninitialized memory), which saturated or blanked the display and survived restart. The buffers are now initialized to the sentinel and `getAutorangeValues()` no-ops unless a real, finite frame exists.
- Waterfall zoom (pinch on Android, Ctrl+scroll on desktop) is now anchored on the frequency under the first finger / mouse cursor instead of the view center, so the signal being tuned stays put on screen while zooming. The Android pinch recognizer keeps the cursor on the first finger (tracked by pointer id) rather than the pinch centroid.
- Scroll wheel over the side menu or other panels no longer also tunes the VFO: the waterfall's hover-area flags were only recomputed while the waterfall was hovered, so a fast mouse exit left them stuck and the main window's wheel-to-tune (and arrow-key tuning) kept firing anywhere. The flags are now cleared whenever the waterfall isn't hovered, and the stuck `digitHovered` flag of the frequency display was fixed too.
- Scroll-wheel tuning and the frequency display digits now work with precision touchpads and free-spinning wheels: fractional wheel deltas were truncated to zero and dropped; they are now accumulated and applied on whole steps.
- The Zoom/Max/Min sliders next to the waterfall now block wheel events from scrolling the panel under them (the upstream `SetItemUsingMouseWheel` call targeted a text label, making it a no-op).
- About (credits) dialog made usable on small touch screens: it previously closed on any touch, so the scrollbar shown when the dialog overflows the screen (e.g. phone landscape) could not be used. The content now scrolls by dragging anywhere in the dialog, the scrollbar is wider and can be grabbed without dismissing, and the dialog closes on a short tap or Escape.
- Desktop menu-width splitter no longer goes dead under the touch style: it gated its hit test on root-window hover, but the hit band straddles child windows whose hover boxes the touch style expands by `TouchExtraPadding`, swallowing the responsive sliver. It now accepts hover from any child of the main window (guarded by `IsAnyItemActive` so drags starting on menu widgets aren't stolen) and keeps the resize cursor for the whole drag. Window splitters also no longer jump to the grab point (this removes a small cursor-offset jump with the mouse on desktop too).
- The touch style overlay is now re-applied on a runtime theme switch (`ThemeManager::applyTheme()` resets rounding to zero, which previously dropped the overlay).
- KiwiSDR map: the selected-server info labels can no longer grow a scrollbar and let the wheel/touch drag scroll the map out of place; overflow is clipped instead, panning stays with the map's own drag handling.
- SDRplay: a no-op PPM device update (`sdrplay_api_Update_Dev_Ppm`) is now skipped when the value hasn't actually changed.
- Android: the drag-scroll recognizer's per-frame `tick()` bails out early when idle instead of calling `steady_clock::now()` every render frame; the Frequency-page keypad respects the source tuning range; and an out-of-bounds `digits[]` write when several typed characters arrived in one frame near the last digit was fixed.
- Minor: the baseband NR CPU-load widget ID stabilized; the source-offset input is hidden in the no-offset mode.

## v1.2.2 - 2026-07-14

First public release of the fork. Cumulatively summarized in [changelog.md](changelog.md); the same code as v1.2.2-beta3.

## v1.2.2-beta3 - 2026-07-14

### Added

- KiwiSDR: "Recent" servers list — the last 8 servers connected to (whether picked on the map, typed by hand or re-picked from the list itself) are remembered across sessions together with their location and served frequency band, for quick reconnection without going through the map.
- Level meter: clicking the meter toggles between a dBFS scale (0 dB at the right edge, negative to the left) and the original positive scale; tick labels and the numeric readout follow the active mode.

### Changed

- SNR readout reworked: SNR is now reported as the held peak in-band level over the out-of-VFO noise floor averaged over the same peak-hold window, so the value stays steady on keyed CW instead of sagging between dits and dashes. The separate "SNR Smoothing" display option was removed — smoothing of the readout is now intrinsic.
- Level meter layout: on narrow windows the meter now switches to sparse (every-other) tick labels and hides the numeric readout instead of overlapping labels, and it can grow wider on large windows.
- Application icons updated with SDRIAK branding, including the macOS and Windows icon variants.

### Fixed

- Fixed an out-of-bounds read in the FFT signal-level calculation (`calculateVFOSignalInfo`): with the VFO at the upper band edge the peak search read one float past the end of the FFT row and crashed the FFT thread (inherited from upstream).
- Fixed a startup crash-loop on Android caused by the SNR smoothing config keys being read without registered defaults (resolved by the removal of that option).
- USB streaming teardown was made sound across all libusb-based sources, fixing use-after-free crashes when stopping a source — most visibly the SIGSEGV in the Airspy HF+ source after a USB unplug on Android:
  - On Android, unplugging a device no longer closes the USB file descriptor under a live libusb handle; the connection is kept alive until the native side releases it, so cancelled transfers are reaped cleanly and a replug cannot be handed a stale connection.
  - The QMX Android USB backend's transfer teardown was reworked (submission/cancellation serialized, exact in-flight accounting, bounded 500 ms drain, leak-instead-of-free if the drain expires) and serves as the reference implementation.
  - The same defect class was fixed in the bundled drivers via PR-ready patches: libairspy, libairspyhf, libhydrasdr and librtlsdr all gained exact in-flight transfer accounting, resubmission gating during cancel, a verified cancellation drain, and a latch-and-leak fallback instead of freeing memory libusb still references. Also fixes rare stop-time crashes on desktop. An upstream librtlsdr hang (`rtlsdr_close()` spinning forever after an error-path exit from async reading) was fixed along the way.
- KiwiSDR map fixes: clicks can no longer select markers that aren't drawn (hidden full servers in Hide Full mode) or leak through a marker to pan the map, overlapping markers now offer a pick-one popup instead of silently choosing the topmost, the Zoom In/Out buttons zoom around the viewport centre, pinch-to-zoom works on Android, the served band is shown in MHz, and on phone-landscape layouts the Test button stays reachable in a fixed footer.

## v1.2.2-beta2 - 2026-07-11

### Breaking change
- Due to the change in network protocol (authentication, source tuning range synchronization), the SDRIAK network protocol is no longer compatible with upstream SDR++. SDRIAK therefore rejects connections from non-SDRIAK SDR++ forks.

### Added

- Dragon Labs source module, merged from upstream SDR++: CR8-1725 device discovery/selection through `libdlcr`, 12.5 MS/s input, internal/external clock selection, channel selection, and LNA/mixer/VGA gain controls.
- Radio squelch mode system from upstream SDR++: `Power`, `CTCSS (Mute)` and `CTCSS (Decode Only)` modes, a full CTCSS tone table with `Any`, received-tone diagnostics, and radio-module interface commands for squelch mode/level, CTCSS tone and high-pass control.
- Recorder filename timestamps can now be generated in either local time or UTC.
- SDRIAK server password authentication: protocol hello/fork compatibility checks, PBKDF2/HMAC challenge-response ported from SDRPlusPlusBrown by @sannysanoff, per-server saved auth keys, explicit server-busy/auth-failed dialogs, and a "Forget saved password" control.
- Shared RX prebuffer implementation for networked sources. The SDRIAK server source now exposes Disabled/50/100/250/500/1000 ms RX prebuffer choices, with Disabled acting as a true live bypass instead of a zero-length buffering path.
- Android release CI now builds a signed universal Play Store `.aab` and splits the universal APK into the published arm and x86 APK artifacts.

### Changed

- Merged the current upstream SDR++ master into this fork, preserving the fork-specific modules and dependency/build-system work while bringing in upstream radio, recorder, rigctl, Dragon Labs, RDS and CI fixes.
- Source tuning limits are now managed by `SourceManager`, are aware of the tuning offset used for up/downconverters, and are forwarded through the SDRIAK server protocol to remote clients.
- The main frequency selector is now constrained to the valid source range for Airspy (24 MHz-1.75 GHz), Airspy HF+ (0-260 MHz), RTL-SDR (100 kHz-1.75 GHz), QMX/QMX server (100 kHz-60 MHz), KiwiSDR served ranges, and remote SDRIAK server sources.
- KiwiSDR tuning limits now follow the selected directory entry's served band and refine to the exact server-reported `freq_offset + bandwidth` range while connected.
- WFM RDS can now disable incremental PS/radio-text updates, showing only complete updated strings when that option is off.
- `SmGui` gained separator and formatted-text helpers with bounded buffers.
- Linux CI now includes Ubuntu Resolute builds.

### Fixed

- SDRIAK server client/session handling was hardened: per-session buffers prevent displaced clients from corrupting a new connection, handshakes must complete before a client can take over the live slot, heartbeats reclaim orphaned sessions, failed authentication is rejected immediately, and malformed packets, command payloads and decompressed baseband sizes are validated before use.
- SDRIAK server remote state synchronization now applies pushed sample-rate and tuning-limit changes on the GUI thread and re-applies them when reselecting the source.
- SDRIAK server and KiwiSDR live prebuffer switching now cleanly swaps between buffered and live paths without leaving stale buffering state behind.
- Rigctl server `get_mode` responses now include the selected VFO bandwidth when available.
- WFM RDS continuation/stale-text handling fixes from upstream were merged.
- HydraSDR source/build compatibility fixes for newer `libhydrasdr` and Windows builds were merged from upstream.

## v1.2.2-beta1 - 2026-07-07

### Added

- Manual/auto AGC switch for the SSB (USB/LSB/DSB), CW/CW-R and AM demodulators, adopted from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73: an AGC checkbox with a gain slider in dB (for AM an Off/Carrier/Audio AGC mode selector). In auto mode the slider shows the live AGC gain; in manual mode it sets a fixed gain, with the last AGC gain carried over so the audio level doesn't jump. Manual gain is still clipped to the maximum output amplitude to protect ears and speakers.
- PlutoSDR improvements adopted from the [SDRPlusPlus](https://github.com/F5OEO/SDRPlusPlus) fork by @F5OEO (Evariste Courjaud): RX1/RX2 RF input selection for hardware with a second RX input (Pluto+, ANTSDR, LibreSDR RevC), and buffer underflow / ADC overload status indicators in the source menu. A config persistence bug in the original RF input selection was fixed during the port; the CS8 streaming mode (Tezuka firmware only) and the removal of the FIR-based low sample rates were deliberately not adopted.

- FFT windows extended from 3 to 7, adopted from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73: Rectangular, Hamming, Hann, Blackman, Nuttall, Blackman-Harris 4-term (−92 dB sidelobes) and Blackman-Harris 7-term (−180 dB sidelobes, for spur hunting next to strong carriers).

- Recorder: FLAC container and 24-bit PCM sample type, adopted from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73. FLAC compresses baseband IQ losslessly to roughly half the size (integer sample types only; sample rates up to 1.048 MHz — a libFLAC format limit — with higher rates failing cleanly at record start). Recording uses libFLAC 1.5.0, statically bundled on Windows/Android/macOS/AppImage and the distro package on Linux .deb builds. The MP3 (LAME) recording option from the same fork was not adopted.
- Recorder: Opus lossy audio recording (`.opus`, Ogg-Opus container), in the spirit of the MP3 option from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73 but using the royalty-free Opus codec instead of MP3. Available for demodulated audio at 8/12/16/24/48 kHz (Opus's native rates), with a bitrate slider (16–256 kbps, default 128). Uses libopus 1.5.2 + libogg 1.3.6, statically bundled on Windows/Android/macOS/AppImage and distro packages on Linux .deb builds.
- File source: playback of finalized FLAC recordings, the read counterpart to the recorder's FLAC container. `.flac` files are streamed through libFLAC's decoder and presented to the rest of the module exactly as an equivalent PCM WAV would be (8/16/24/32-bit, mono/multi-channel), so all existing handling — frequency-from-filename parsing, looping, the format/duration readout — applies unchanged. The reader is picked from the file's magic bytes, not its extension, and the file dialog now lists `*.wav` and `*.flac`. The FLAC decoder lives in core next to the recorder's encoder (libFLAC stays a private core dependency). Unlike the WAV reader, an unfinalized FLAC left by a crashed recorder (no total sample count in its header) is not recovered — a compressed stream's length can't be derived from the file size — and is reported as unplayable rather than mis-timed.

### Changed

- FFT amplitude calibration (also from @qrp73): the FFT window is now normalized to unity coherent gain, so a sinusoid reads its true dBFS amplitude with any window, any FFT size/rate combination, and any zero padding. Previously the reading was only correct for the rectangular window, and the displayed level also sagged when the FFT rate was raised. **With the default Nuttall window all FFT/waterfall levels now read ~9 dB higher (Blackman: ~7.5 dB), so saved waterfall min/max ranges will need a one-time re-adjustment.** The noise floor still shifts slightly between windows (equivalent noise bandwidth) — that is physically correct and matches bench spectrum analyzers.
- The SNR meter in the top bar was replaced by a signal level meter, adopted from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73: a level bar in dBFS (peak FFT level within the VFO bandwidth) with a peak hold marker over the last 10 FFT frames, plus a numeric readout of the peak level and the SNR.
- File source WAV support extended, adopted from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73: plays RF64 (>4 GB recordings), WAVE_FORMAT_EXTENSIBLE, 8/16/24/32-bit PCM and 32/64-bit float, mono and multi-channel files, and recovers unfinalized recordings from crashed recorders. The sample format is auto-detected (the "Float32 Mode" checkbox is gone), the menu shows the detected format and duration, filename frequencies are also parsed with kHz/MHz/GHz units and decimals (HDSDR/SDR Console naming), and selecting a file during playback — previously a crash — is now disabled.
- Squelch improvements adopted from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73: 1 dB hysteresis when closing and a 100 ms above-threshold hold before unmuting, so the squelch no longer chatters at the threshold or pops open on short noise spikes. The hold is sample-count based in this port, exact at any IF sample rate.
- PlutoSDR (also from @F5OEO): larger IIO buffers (50 ms, 8 kernel buffers) to avoid underflows at high sample rates over USB and network, device scan now covers both the USB and network backends, extended device whitelist (Pluto+, ad9361, FISH), and the full libiio description is shown as the device name.

### Fixed

- Squelch level dB math (also from @qrp73): the threshold now uses `20*log10` of the mean signal amplitude instead of `10*log10`, so the slider value is in real dB. Previously saved squelch levels will trigger differently and need re-adjusting.

## v1.2.2-beta - 2026-07-06

### Added

- Frequency manager: merged the improvements from [`bookmark_manager`](https://github.com/darauble/bookmark_manager) by @darauble (Darau Ble), with contributions by @daviderud (Davide Rovelli), directly into the stock frequency manager module (the config format stays backward compatible): waterfall labels arranged in up to 10 rows with overlap avoidance, per-list colors, text-only and flag-style label options, UTC start/end times and week days per bookmark with off-air greying (UTC helpers from `shortwave-station-list-sdrpp` by Otto Pattemore), notes and geo info fields, a sortable bookmark table, click-to-select of waterfall labels, and moving bookmarks between lists. Several issues found in the original were fixed during the merge (crash on malformed list color, buffer overflows on long imported strings and malformed `days` arrays, stale label hit-testing, new bookmarks ignoring the selected target list, and a crash on malformed import files).
- IF noise reduction (LogMMSE/OMLSA) module, ported from SDRPlusPlusBrown by @sannysanoff.

### Changed

- IF noise reduction hardening and refactoring after the port: crash safety and correct attach/detach to the IF chain, TX and samplerate awareness, removal of debug scaffolding and dead code, value-semantics arrays with a trimmed API and opt-in buffer recycling, a generated `expn()` lookup table replacing the hardcoded one, and UI polish.
- Const correctness fixes of `dsp::complex_t`.
- CI: Android Debug APKs are now opt-in and the Build Android workflow can be triggered manually (`workflow_dispatch`).

### Fixed

- IF noise reduction: fixed a buffer overflow in `logmmse_all()` output with large IF bandwidths and an out-of-bounds read in the `expn()` lookup.

## v1.2.2-alpha4 - 2026-07-04

### Added

- New CMake-based dependency build system, ported from PrusaSlicer's (written by Tamas Meszaros, @tamasmeszaros): all third-party libraries are now built from source instead of pulling prebuilt binary blobs. This enables native Windows on ARM64 builds and removes the need for the custom Android SDR-kit Docker image.
- Native Windows on ARM64 builds.
- Linux AppImage builds, with isolated config root.
- Radiosonde decoder plugin, merged directly into SDRIAK from `sdrpp_radiosonde` by @dbdexter-dev (Davide Belloli), built on his `sondedump` decoding library.
- Spots module, merged from [`sdrpp-spots`](https://github.com/gerner/sdrpp-spots) by @gerner.
- WebSDR view module, based on the KiwiSDR map and waterfall code from SDRPlusPlusBrown by @sannysanoff.
- libcurl integration for HTTPS and secure WebSockets, statically bundled and exported through `sdrpp_core`.
- KiwiSDR improvements merged from the `qrp73`/`sdr73` forks by @qrp73: server selection by address and port, server band parsing, and map improvements (day/night terminator, country colors, hover tooltips, and a runtime-toggleable marker style).

### Changed

- Reworked the WebSocket client for RFC 6455 correctness and hardening (handshake validation, fragmentation, redirect handling, and concurrency/write safety).
- Networking hardening and cleanup across the KiwiSDR and web modules.

### Fixed

- QMX on Linux: fixed an exception when enumerating ALSA audio devices (#3).
- Fixed input events leaking through overlapping ImGui windows.
- Fixed `flog` string conversion for all numeric types supported by `to_string`.
- Fixed a Debian package build regression where optimization was disabled.

## v1.2.2-alpha3 - 2026-04-21

### Added

- Merged KiwiSDR support from SDRPlusPlusBrown by @sannysanoff.

### Changed

- Further steps to fork from upstream with SDRIAK branding so that SDRIAK could live side by side with upstream SDR++ on the same system. Fixed Linux Debian packages broken by the previous branding pass.
- Fix of default audio sink on Linux with ALSA audio API selected: Don't let the ALSA audio queue dry up if radio is stopped. The ALSA sink would not restart if the radio was paused and restarted.

## v1.2.2-alpha2 - 2026-04-19

### Changed

- Reworked high DPI display handling: The display DPI is newly respected.
- A new user display scaling factor now multiplies with the display native DPI scale.
- General clean-up of the UI scaling code to respect the display and user scaling factors.
- Expanded the touch region around the view splitters on Android so that they could be touched by thick fingers.

## v1.2.2-alpha1 - 2026-04-19

### Added

- Added Android audio rerouting when the preferred audio output changes.
- Added Ctrl+scroll waterfall bandwidth zooming.
- Added initial Android multi-touch support for waterfall zooming.

### Changed

- Updated CI artifact naming to include version information, commit distance, and commit hash from `git describe`.
- Added the `sdriak` name to release artifacts and Linux Debian packages to keep this fork isolated from upstream SDR++ packages.
- Reworked CI release handling to read version information from `version.h`.

### Fixed

- Fixed Android audio sink behavior by pausing and restarting audio output to work around devices that lock when the audio queue dries up.
