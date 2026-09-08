# Changelog

Major releases only. For the detailed per-release history including alpha and beta pre-releases, see [changelog-full.md](changelog-full.md).

## v1.4.0-beta2 - 2026-09-08

This follow-up beta completes the rename to **SDRIAK**, improves QMX panadapter synchronization, and polishes touch interaction on the waterfall.

### QMX panadapter operation

- With **Sync VFO** enabled, the selected VFO remains at the center of the waterfall at every zoom level while tuning or panning, and frequency changes synchronize in both directions between SDRIAK and QMX.
- QMX and SDRIAK modes now synchronize in both directions. AM is supported by current QMX firmware, and CW/CW-R changes follow the sideband implied by the QMX CW/CWR settings.

### Touch, tuning and interface

- Tuning is now possible by clicking and dragging inside the VFO band if the VFO is already selected ([#21](https://github.com/bubnikv/sdriak/issues/21)).

### SDRIAK identity and packaging

- The former **SDR++ iak** name has been replaced with "SDRIAK" and the icon was changed to differentiate from the SDR++ (to be registered trademark).

## v1.4.0-beta

The version jumps from the 1.2.2 series to 1.4.0-beta because this release builds on upstream SDR++'s continuously evolving 1.3.0 codebase.

A feature and reliability release focused on touch operation, faster tuning and a more robust experience across desktop, Android and server use.

**Alpha testers:** delete the fork's config directory before upgrading. Pre-release numeric radio modes and frequency-memory layouts are not migrated.

### User interface and tuning

- Touch-friendly UI overhaul with Android-sized controls, a Material 3 dark theme, drag-scroll with fling, visible splitter handles and haptic feedback. The touch style can also be previewed on desktop.
- Android Back now dismisses popups and backgrounds the app naturally; holding the hamburger button opens the exit confirmation. Small-screen dialogs and the About view are easier to navigate.
- New Bands / Frequency dialog with an IC-705-style direct-entry keypad, category-filtered band selection and a Spectrum page for moving between named frequency ranges.
- Three-register band stacking remembers frequency and mode per band. Band identities, overlap resolution, defaults and register rotation were stabilized for split and mixed-service band plans.
- Waterfall scaling now uses **Ref + Range**, with robust one-shot auto-fit or continuous tracking over the visible spectrum. The original auto-range was contributed by [@ericek111](https://github.com/ericek111) ([upstream #1729](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1729)); new colormaps were contributed by [@konung-yaropolk](https://github.com/konung-yaropolk) ([upstream #1694](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1694)). Cursor-anchored mouse/pinch zoom was also added.
- Cleaner desktop presentation at fractional scales, including whole-pixel rendering and new 125% and 175% presets ([upstream #1116](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1116), [PR #1115](https://github.com/AlexandreRouma/SDRPlusPlus/pull/1115)); dialogs now share consistent Enter, Escape and click-outside behavior ([upstream #1758](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1758)).
- More reliable mouse-wheel and precision-touchpad input: fractional tuning steps are accumulated correctly and wheel actions no longer leak from menus or sliders into VFO tuning.
- Compact radio-mode selector and editable CW pitch presets.
- Right-click spectrum bookmark creation, contributed by [@Zaryob](https://github.com/Zaryob) from [upstream PR #1476](https://github.com/AlexandreRouma/SDRPlusPlus/pull/1476) ([#1475](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1475)).
- Per-device SDRplay PPM correction, contributed by [@M0OPK](https://github.com/M0OPK) ([upstream #1781](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1781)).
- International RDS text rendering based on work by [@attah](https://github.com/attah) ([upstream #1164](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1164)).

### Platforms and connectivity

- Native macOS CoreAudio output sink, ported from SDR++Brown by [@sannysanoff](https://github.com/sannysanoff), with UTF-8 device names, device-rate tracking and a safe null-output fallback ([upstream #1776](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1776)).
- SDRIAK Server and SpyServer connections are asynchronous, cancellable and bounded by DNS/connect timeouts; the remaining blocking TCP paths also gained finite timeouts and clearer errors. The original work was contributed by [@edudant](https://github.com/edudant) in [fork PR #20](https://github.com/bubnikv/sdriak/pull/20), resolving [fork #19](https://github.com/bubnikv/sdriak/issues/19).
- Server and rigctl handling was hardened against dead links, socket leaks and shutdown races. Hamlib interoperability now includes additional VFO tokens, receive-only PTT replies and CWR capability reporting (upstream [#1462](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1462), [#1061](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1061), [#1506](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1506), [#1092](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1092)).
- Relative config, module and resource paths now resolve from the executable instead of the launch directory ([upstream #1265](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1265)). Windows builds use UTF-8 console handling and flush logs reliably; the logging fix was ported from SDR++Brown with [@sannysanoff](https://github.com/sannysanoff).

### Reliability and compatibility

- Configuration access now uses scoped transactions throughout the application. Unchanged settings are not rewritten, malformed values fall back safely, shutdown flushes pending saves and concurrent desktop instances merge edits more reliably.
- Radio modes, bookmarks, the selected demodulator and band memories now persist using stable names and identities rather than internal enum ordering. Explicit bookmark import remains compatible with upstream numeric modes.
- Missing or unusable audio devices no longer crash startup or stall the DSP pipeline ([upstream #1754](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1754)); short audio reads are padded safely using a fix ported from SDR++Brown with [@sannysanoff](https://github.com/sannysanoff), and partially opened streams are closed before fallback.
- Fixed crashes and races involving window resizing, scanner restarts, dynamic menu creation, concurrent mode changes and server-mode source panels (upstream [#1658](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1658), [#816](https://github.com/AlexandreRouma/SDRPlusPlus/issues/816), [#1437](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1437), [#1630](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1630)).
- Fixed an Android RS41 radiosonde decoder abort caused by a serial-field over-read. Thanks to [@jprincl](https://github.com/jprincl).
- Additional fixes cover malformed band-plan/resource files, configuration deadlocks and Windows save interference, stale waterfall auto-range data, socket lifetime and several DSP boundary/race conditions.

## v1.2.2 - 2026-07-14 — first public release (then named SDR++ iak)

The first public release, then named **SDR++ iak**, was maintained by Vojtech Bubnik (OK1IAK) from March 2026. It builds on [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) by Alexandre Rouma (@AlexandreRouma) and stays merged with the current upstream master. The fork installs side by side with upstream SDR++ (own package names, config directory and Android app ID). This entry summarizes everything since the fork.

### QRP Labs QMX transceiver support — the reason this fork exists

- Direct USB IQ source for the [QRP Labs QMX](https://qrp-labs.com/qmx) transceiver by Hans Summers (G0UPL), with native backends for Windows, Linux, macOS and Android: 48 kS/s 24-bit IQ streaming, bidirectional CAT synchronization of VFO frequency and mode (including CW-R), and audio muting while the QMX transmits.
- QMX server source for remote reception: IQ streamed over the network from a QMX attached to a remote box, e.g. the companion [Android server app](https://github.com/bubnikv/qmxserver-android), using the [zpl-c/enet](https://github.com/zpl-c/enet) library (Lee Salzman's enet with IPv6, extended for this fork).

### Android overhaul

- Modernized toolchain (Kotlin, Gradle, current NDK, SDK 36, Java 17), signed release builds, and Play Store packaging (universal `.aab` plus arm/x86 APKs) built by CI.
- Audio sink rewritten on Oboe (Android 7+ compatibility), audio rerouting on output change, robust suspend/wake and keep-alive handling, multi-touch waterfall zoom, and a reworked high-DPI/user display scaling system.
- Reworked USB device access shared by all USB source modules.

### New sources and modules

- KiwiSDR client, merged from SDRPlusPlusBrown by @sannysanoff and extended: a world-map server directory with day/night terminator and server tooltips (map improvements from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73), recent-servers list, served-band tuning limits and RX prebuffering.
- WebSDR view module, based on KiwiSDR map/waterfall code from SDRPlusPlusBrown by @sannysanoff.
- Radiosonde decoder, merged from [`sdrpp_radiosonde`](https://github.com/dbdexter-dev/sdrpp_radiosonde) by @dbdexter-dev (Davide Belloli), built on his `sondedump` library.
- Spots module, merged from [`sdrpp-spots`](https://github.com/gerner/sdrpp-spots) by @gerner.
- Dragon Labs CR8-1725 source and the CTCSS squelch mode system, merged from upstream SDR++.

### Radio and DSP improvements

- Adopted from the [SDRPP](https://github.com/qrp73/SDRPP) fork by @qrp73: manual/auto AGC for SSB/CW/AM, seven FFT windows (up to Blackman-Harris 7-term), FFT amplitude calibration to true dBFS, a signal level meter with peak hold replacing the SNR meter, squelch hysteresis/hold and a squelch dB-math fix.
- IF noise reduction (LogMMSE/OMLSA) module, ported from SDRPlusPlusBrown by @sannysanoff, then hardened and refactored.
- PlutoSDR improvements adopted from the fork by @F5OEO (Evariste Courjaud): RX1/RX2 input selection, larger IIO buffers, underflow/overload indicators.
- Frequency manager merged with [`bookmark_manager`](https://github.com/darauble/bookmark_manager) by @darauble (Darau Ble), with contributions by @daviderud (Davide Rovelli) and UTC helpers by Otto Pattemore: multi-row waterfall labels, per-list colors, scheduled on/off-air bookmarks, sortable table, and many robustness fixes made during the merge.

### Recording and playback

- FLAC baseband recording (adopted from @qrp73's fork) and FLAC playback in the file source; Opus lossy audio recording; 24-bit PCM; extended WAV support (RF64, float, multi-channel, crash recovery — also from @qrp73's fork).

### SDRIAK Server (then called SDR++ Server)

- Password authentication (PBKDF2/HMAC challenge-response, ported from SDRPlusPlusBrown by @sannysanoff), source tuning-range synchronization to remote clients, configurable RX prebuffer, and extensive session/protocol hardening. **The network protocol is no longer compatible with upstream SDR++.**

### Build system, packaging and reliability

- New CMake dependency build system ported from PrusaSlicer (by Tamas Meszaros, @tamasmeszaros): all third-party libraries built from source — enabling native Windows ARM64 builds and Linux AppImages, with no prebuilt binary blobs.
- USB streaming teardown made sound across all libusb-based sources (use-after-free fixes in the QMX Android backend plus PR-ready patches for libairspy, libairspyhf, libhydrasdr and librtlsdr), and numerous crash fixes throughout (FFT edge reads, config parsing, Android audio).

Thanks to Alexandre Rouma and all upstream SDR++ contributors, and to @sannysanoff, @qrp73, @darauble, @dbdexter-dev, @gerner, @F5OEO and @tamasmeszaros, whose work this release builds on.
