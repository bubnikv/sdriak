# Upstream bugs fixed and features implemented in SDRIAK

Bugs that Vojtech Bubnik fixed in SDRIAK and that are not yet fixed in [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus), and [features requested on the upstream tracker](#features-requested-upstream-implemented-in-sdriak) that SDRIAK implements, collected from the commit log and the release logs.

Every entry was checked against the code of upstream commit `ea6f755a` (2026-11-06) and the issue tracker on 2026-11-07. Entries that upstream has resolved since are removed from this list.

The bug lists leave out fixes adopted from other people's work, such as the squelch level fix from [qrp73](https://github.com/qrp73/SDRPP). Those are credited in the [readme](../readme.md#credits) and the [changelog](../changelog.md).

## Fixed in SDRIAK

| Upstream issue | Bug | Fix | First in |
| --- | --- | --- | --- |
| [#816](https://github.com/AlexandreRouma/SDRPlusPlus/issues/816) | Adding a module from the Module Manager could crash the app: the menu loop kept iterating a vector that a draw handler had just reallocated. | `2c2c9def` | v1.4.1-alpha |
| [#1061](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1061) | The rigctl server stopped accepting connections after tens of thousands of connects: every client-initiated disconnect leaked a socket descriptor. | `d74af11f` | v1.4.0-beta |
| [#1092](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1092) | WSJT-X reported a rig failure: Hamlib VFO tokens such as `F VFOA 7074000` were rejected. The issue was closed upstream in 2024, but the tokens are still not handled there. | `a4630b0a` | v1.4.0-beta |
| [#1462](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1462) | The client froze on a network interruption: it blocked forever on a dead server link while still showing Connected. | `12396d3f` | v1.4.0-beta |
| [#1506](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1506) | Rigctl problems left after upstream fixed the `get_mode` timeout: no PTT replies for WSJT-X, an uninitialized mode with no demodulator selected, and a bandwidth cast from NaN. | `a4630b0a` | v1.4.0-beta |
| [#1630](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1630) | The server crashed when a client selected the Hi-Z antenna of an RSP2: source menus called ImGui directly, and server mode has no ImGui context. | `3c8ed066` | v1.4.0-beta |
| [#1658](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1658) | The scanner crashed the app on restart or removal: a worker that had exited by itself was never joined, which calls `std::terminate`. | `90eed661` | v1.4.0-beta |

## Partly fixed in SDRIAK

| Upstream issue | Bug | Fix | First in |
| --- | --- | --- | --- |
| [#1437](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1437) | ImGui assertion after long rigctl sessions: the network thread changes radio state in the middle of a frame. The reported crash is fixed, but rigctl still changes GUI state from the network thread, see [ui-thread-sync.md](bugs/ui-thread-sync.md). | `b3664297` | v1.4.0-beta |

## Open upstream, fix not confirmed

These issues are open upstream. SDRIAK changed the same area, but the changes are not confirmed to fix them.

| Upstream issue | Bug | Change | First in |
| --- | --- | --- | --- |
| [#1265](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1265) | Crash on macOS when started by double click. Relative paths now resolve from the executable instead of the launch directory. | `1576b30f` | v1.4.1-alpha |
| [#1313](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1313), [#1654](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1654) | HackRF crashes on Android. USB teardown and error handling were hardened. | `e5a00dcd` | Not released yet |

## Features requested upstream, implemented in SDRIAK

None of these are in the upstream code. The **Based on** column names the person whose request, patch or pull request the feature builds on.

| Upstream request | Feature | Commit | Based on | First in |
| --- | --- | --- | --- | --- |
| [#1008](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1008) | Theme selector moved into the Display panel. | `1accb3db` | | v1.4.0-alpha |
| [#1116](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1116), [PR #1115](https://github.com/AlexandreRouma/SDRPlusPlus/pull/1115) | Fractional UI scaling: 125% and 175% presets, with whole-pixel fonts and waterfall geometry so the UI stays crisp. | `44d39169`, `5852b3cc`, `02e73457` | | v1.4.1-alpha |
| [PR #1164](https://github.com/AlexandreRouma/SDRPlusPlus/pull/1164) | RDS text decoded from the EBU character set to UTF-8, for international station names. | `1b674d52` | [attah](https://github.com/attah) | v1.4.0-alpha |
| [#1475](https://github.com/AlexandreRouma/SDRPlusPlus/discussions/1475), [PR #1476](https://github.com/AlexandreRouma/SDRPlusPlus/pull/1476) | Right-click on the FFT or waterfall adds a bookmark at the cursor frequency. | `372780d4` | [Zaryob](https://github.com/Zaryob) | v1.4.0-alpha |
| [#1694](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1694) | Six new waterfall colormaps. | `36f8a2b5` | [konung-yaropolk](https://github.com/konung-yaropolk) | v1.4.0-alpha |
| [#1729](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1729) | Automatic FFT and waterfall range, one-shot or continuous. | `082e73e3` | [ericek111](https://github.com/ericek111) | v1.4.0-alpha |
| [#1758](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1758) | Enter and Escape apply and dismiss the frequency manager dialogs. | `cc63354a` | | v1.4.0-alpha |
| [#1781](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1781) | Per-device PPM correction for SDRplay. | `d3e8b764` | [M0OPK](https://github.com/M0OPK) | v1.4.0-alpha |

