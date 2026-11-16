# Demodulator mode representations across the app

The runtime ground truth is the integer `RADIO_IFACE_MODE_*`
(`core/src/radio_interface.h`) — that is what travels over module-com. Every
display and storage context spells a mode through one table,
`RADIO_IFACE_MODE_NAMES[]`, with a single exception for the Hamlib wire
protocol and a rig-defined digit code for QMX CAT.

## Persistence

Local persisted state uses the canonical name: band-stack registers store
`mode: "USB"` (and omit an unresolved mode), the radio stores
`selectedDemod: "USB"`, and bookmarks store `mode: "USB"` (also omitted when
unresolved). None of these sites reads numeric values.

| Site | Canonical write | Invalid or obsolete input |
|---|---|---|
| module-com | integer `RADIO_IFACE_MODE_*` | runtime ABI |
| `config.json` band-stack register | `mode: "USB"`; absent when unresolved | treat as unresolved |
| `radio_config.json` selected demodulator | `selectedDemod: "USB"` | default to WFM and repair the string |
| frequency-manager config and bookmark export | `mode: "USB"`; absent when unresolved | treat as unrecorded and leave the active demodulator alone |
| explicit bookmark import | accept canonical strings and range-checked SDR++ integers | normalize into the string-only internal format; report invalid modes |

Input comparison is ASCII case-insensitive; output always uses the canonical
table spelling. Numeric modes are accepted only at the explicit external
bookmark-import boundary.

The per-mode profile objects beneath each radio instance are a separate matter:
their keys come from the canonical `getName()` output and JSON object-key lookup
is case-sensitive. Thus `selectedDemod: "usb"` resolves to USB, but a hand-added
profile object named `"usb"` does not replace the canonical `"USB"` profile.

## The table

| # | enum | `RADIO_IFACE_MODE_NAMES` | radio menu button | demod `getName()`<br>= config key | freq_manager | recorder `$r` | discord | band stack | rigctl_server<br>**= Hamlib wire** | QMX CAT |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `NFM` | NFM | NFM | NFM | NFM | NFM | NFM | NFM | **FM** | — |
| 1 | `WFM` | WFM | WFM | WFM | WFM | WFM | WFM | WFM | WFM | *(parse only)* |
| 2 | `AM` | AM | AM | AM | AM | AM | AM | AM | AM | *(parse only)* |
| 3 | `DSB` | DSB | DSB | DSB | DSB | DSB | DSB | DSB | DSB | — |
| 4 | `USB` | USB | USB | USB | USB | USB | USB | USB | USB | `MD2` |
| 5 | `CW` | CW | CW | CW | CW | CW | CW | CW | CW | `MD3` |
| 6 | `LSB` | LSB | LSB | LSB | LSB | LSB | LSB | LSB | LSB | `MD1` |
| 7 | `RAW` | RAW | RAW | RAW | RAW | RAW | RAW | RAW | RAW | — |
| 8 | `CWR` | CWR | CWR | CWR | CWR | CWR | CWR | CWR | CWR | `MD7` |

`radioModeName()` reads the table and `radioModeFromName()` parses it,
accepting `"FM"` and `"CW-R"` on input only, so a hand-written band plan
`def_mode` or a config from another program still resolves. Every
`demod::*::getName()` returns `radioModeName(RADIO_IFACE_MODE_*)`, so the
per-mode config keys in `radio_config.json` are the canonical names by
construction.

Every column is the same string, read from the one table, with two protocol
exceptions:

- **rigctl_server** must speak Hamlib, where `RIG_MODE_FM` is documented as
  `/*!< FM -- "narrow" band FM */`. `hamlibModeName()` / `hamlibModeFromName()`
  (`rigctl_server/src/main.cpp`) are one `if` each over the canonical table.
  Hamlib's `CWR` needs no exception — it is the canonical spelling. Input is
  otherwise left to `radioModeFromName()`, lenient aliases included: a client
  that sends `NFM` means the same mode the server reports as `FM`. The `M ?` /
  `\set_mode ?` capability reply is built by `hamlibModeList()` from the same
  table, so the advertised list, the accepted list and the reported name
  cannot diverge.
- **QMX CAT** is Kenwood TS-480 `MD<digit>`, fixed by the rig, and bridges only
  four modes (`LSB`, `USB`, `CW`, `CWR`) through
  `FreqModeSync::qmxModeToRadioIface()`. `FM`/`AM` exist in `qmx::QmxMode`
  because the `MD`/`IF` responses can carry them, but QMX never sends them and
  `encodeModeCommand()` refuses them. The source's front-panel label routes
  those four through the same mapping and then `radioModeName()`, so the panel
  cannot name a mode differently from the radio menu it is driving. Only
  `FSK`/`FSKR` stay a local switch — they are rig modes with no demodulator
  here, so no canonical name exists for them.

The **radio menu owns its ordering** — `NFM WFM AM DSB RAW / LSB USB CW CWR`,
so the default 5+4 button wrap groups related modes — but the labels are built
from `radioModeName(modeIDs[i])`, so a button cannot disagree with the name
used elsewhere.

## Where each representation lives

| Spelling | Defined at |
|---|---|
| canonical | `core/src/radio_interface.h` — `RADIO_IFACE_MODE_NAMES[]`, `radioModeName()`, `radioModeFromName()` |
| radio menu | `radio_module.h` — `modeIDs` order only; labels via `radioModeName()` |
| demod name | `demodulators/*.h` — `getName()` returns `radioModeName(RADIO_IFACE_MODE_*)` |
| rigctl | `rigctl_server/src/main.cpp` — `hamlibModeName()` / `hamlibModeFromName()` / `hamlibModeList()` |
| freq_manager | `frequency_manager/src/main.cpp` — `radioModeName()`, combo over `RADIO_IFACE_MODE_NAMES` |
| recorder | `recorder/src/main.cpp` — `radioModeName()` |
| discord | `discord_integration/src/main.cpp` — `radioModeName()` |
| band stack | `core/src/gui/widgets/band_stack.*`, `gui/widgets/freq_input/bands.cpp` — `radioModeName()` |
| QMX wire | `libqmx/src/QmxCatStatus.cpp` — `decodeModeChar`, `encodeModeCommand` (own enum, rig-defined digits) |
| QMX panel label | `qmx_source/src/main.cpp` — `qmxModeToRadioIface()` + `radioModeName()`; `FSK`/`FSKR` local |
| band plan `def_mode` | `root/res/bandplans/*.json` + `scripts/enrich_bandplans.py` |

`misc_modules/rigctl_client` handles frequency only and has no mode
representation. `sdrpp_server` does not carry modes (IQ only).

Code that reads a mode from another module guards against a missing reply:
`callInterface()` invokes no handler when the interface is not registered, and
the radio ignores `GET_MODE` while `selectedDemod` is NULL. The frequency
manager seeds its query with `RADIO_IFACE_MODE_RAW` and reduces an unusable
bookmark mode to `-1`, never to `RAW`, which is a real demodulator a bookmark
must only request when the user chose it. `bookmarkFromJson()` checks each
field's presence and type, since `j["frequency"]` on a *const* `json` with a
missing key is undefined behaviour rather than a throw.

## What is free to change, and what is not

**Locked — persisted.** Changing these silently discards user data:

- **Demod `getName()` is the config key** for every per-mode profile:
  `config.conf[<vfo>][<name>]{bandwidth, snapInterval, squelchLevel, squelchMode,
  ctcssTone, highPass, deemphasis, FMIFNR, noiseBlanker}` (`radio_module.h`).
  Renaming a mode orphans its profile unless a migration renames the keys
  first. The string is never displayed and never leaves `radio_config.json`,
  a module-local `ConfigManager` that nothing outside the radio module opens;
  nothing iterates its keys, so every access is an exact lookup.
- **The canonical strings in `config.json` and the frequency-manager files.**

**Locked — protocol.** Changing these breaks third parties:

- **rigctl_server** speaks Hamlib, where `FM` *is* the name for narrow FM and
  `WFM` is separate. It also **parses** these names, so the table is
  bidirectional.
- **QMX CAT** is Kenwood TS-480 `MD<digit>`, fixed by the rig.

**Free — display only.** Nothing reads these back: radio menu button labels,
the frequency-manager bookmark list, hover tooltip and edit combo, the recorder
`$r` filename substitution, the Discord rich-presence text, and the band-stack
register list.

**Effectively free — resources we ship.** Band plan `def_mode` is a string in
`root/res/bandplans/*.json`, parsed by `radioModeFromName()`. Users can
hand-edit plans, so the parser stays permissive.

**Adding a mode** means one row in `RADIO_IFACE_MODE_NAMES[]` (appended, so
existing numeric values stay stable on module-com), a demodulator whose
`getName()` returns `radioModeName()` of it, a button position in the radio
menu's `modeIDs`, and a decision in each protocol adapter: a Hamlib name in
`hamlibModeName()`, and an `MD` digit only if the QMX rig supports it.

## Why these spellings

A single table cannot cover everything, because narrow FM must be spelled
`"FM"` on the Hamlib wire and `"NFM"` in the UI, in the same build. So there is
one canonical table for every display and storage context, plus two protocol
adapters (Hamlib in rigctl_server, Kenwood `MD` in libqmx) where a foreign
vocabulary is fixed.

**`NFM` / `WFM`** is the receiver convention. A ham transceiver only
*transmits* narrow FM, so "FM" needs no qualifier there and "WFM" is added for
broadcast reception; a receiver has no transmit bias and names both
explicitly:

| Software / rig | Narrow FM | Broadcast FM |
|---|---|---|
| **SDR#** (SDRSharp) | `NFM` | `WFM` |
| **SDRangel** | `NFM` | `WFM` |
| **SDRuno** (SDRplay) | `NFM` | `WFM` (+ `MFM`, `SWFM`) |
| **GQRX** | `Narrow FM` | `WFM (mono)` / `WFM (stereo)` |
| **SDRIAK** | `NFM` | `WFM` |
| — | | |
| **Hamlib** | `FM` ("narrow" band FM) | `WFM` (broadcast wide FM) |
| **Icom IC-705** | `FM` | `WFM` (RX only) |
| **Kenwood TS-990S** (via Hamlib) | `FMN` | — |

SDRIAK is a receiver whose users come from SDR#, SDRangel, GQRX and SDRuno, so
it uses their spelling; Hamlib's `FM` stays behind rigctl_server's adapter. No
in-tree vocabulary uses plain `FM` for broadcast FM, which would invert
Hamlib's meaning.

**`CWR`** matches Hamlib's `RIG_MODE_CWR` and `qmx::QmxMode::CWR`, so the
rigctl adapter needs no exception for it. `CW-R`, the IC-705 spelling, is
accepted on input.

## Sources

- [Hamlib `include/hamlib/rig.h`](https://github.com/Hamlib/Hamlib/blob/master/include/hamlib/rig.h) — `RIG_MODE_FM` "narrow band FM", `RIG_MODE_WFM` "broadcast wide FM", `RIG_MODE_CWR`, `RIG_MODE_FMN`
- [SDR# user guide](https://manuals.plus/m/8241aadafb531474a3ef7a3e0c0fe20aa31a2222ecf1f6b146e9208066b77a2c) — NFM / WFM / AM / LSB / USB / DSB / CW
- [SDRangel NFM demodulator plugin](https://github.com/f4exb/sdrangel/blob/master/plugins/channelrx/demodnfm/readme.md) — NFM and WFM demodulator plugins
- [Gqrx documentation](https://www.gqrx.dk/doc/practical-tricks-and-tips) — "Narrow FM", "WFM (mono)", "WFM (stereo)"
- [SDRuno user manual](https://www.sdrplay.com/docs/SDRplay_SDRuno_User_Manual.pdf) — NFM / MFM / WFM / SWFM
- [IC-705 Basic Manual](https://www.icomeurope.com/wp-content/uploads/2024/06/IC-705_ENG_IM_Basic_7.pdf) and [Icom IC-705 product page](https://www.icomjapan.com/lineup/products/IC-705/) — SSB / CW / RTTY / AM / FM / DV, plus WFM for broadcast reception
