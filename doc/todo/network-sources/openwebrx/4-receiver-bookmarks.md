# OpenWebRX+ 4: the receiver's bookmarks on the waterfall

Date: 2026-11-25. Status: current. Nothing implemented. Part of
[README.md](README.md). Prerequisite: OpenWebRX+ 2.

Each profile change, the server sends the receiver socket three lists for
the profile's span (`owrx/connection.py`, `sendBookmarks` and
`sendBands`). The client receives them today and ignores them:

| message | entries | where they come from |
|---------|---------|----------------------|
| `bookmarks` | `{name, frequency, modulation, underlying, description, scannable}` | the owner's bookmarks; EIBI broadcast schedules within `eibi_bookmarks_range` km; RepeaterBook repeaters within `repeater_range` km. The last two are off unless the owner sets a range |
| `dial_frequencies` | `{frequency, mode}`, e.g. `{144489000, "wspr"}` | the server's bandplan: FT8, FT4, WSPR, JS8 and the other digimode dials |
| `bands` | `{name, low_bound, high_bound, tags}` | the server's bandplan |

This step shows the first two on the waterfall as read-only bookmark
lists, through the frequency manager. Its label drawing, click-to-tune,
display mode and colours then serve them unchanged.

`bands` is not used: our own bandplan already draws band edges, and a
second, differing set from the server would only compete with it.

## Core: source bookmarks

* **New `core/src/gui/source_bookmarks.h`.** A holder that sources fill and
  modules read:
  * `struct SourceBookmark { name; frequency; mode; bandwidth; description; }`.
    `mode` is a `RADIO_IFACE_MODE_*`, or −1 when the server's mode has no
    counterpart here. `bandwidth` of 0 means "keep the VFO's".
  * `set(list, entries)` and `clear(list)`. Any thread: the network thread
    writes, the UI thread reads. A mutex guards them.
  * `snapshot()`, and a `generation()` counter that every change bumps.
  * A list's name says what it is, e.g. "OpenWebRX+ bookmarks" and
    "OpenWebRX+ dials".
* GUI only, like `SpectrumProvider`: nothing in `--server` mode.
* Kept generic on purpose. Other servers have such lists too; KiwiSDR's DX
  labels are the likely next user (not yet read from its source).

## Frequency manager

* **Source lists appear among the user's lists**, marked read-only:
  * not saved into `lists`;
  * no edit, rename or delete;
  * "Copy to list..." saves chosen entries into a user list.
* **On the waterfall** (persisted, one flag per list name): on for "dials",
  off for "bookmarks". EIBI alone can return hundreds of entries for a
  shortwave profile.
* Their own colour, by default distinct from the user lists' yellow.
* `refreshWaterfallBookmarks()` also runs when `generation()` changes. It
  is checked once per frame, on the UI thread.
* Clicking a label tunes the selected VFO, as for any bookmark. That sets
  the mode and bandwidth when they are known.

## The source

* **When.** The client parses `bookmarks` and `dial_frequencies`. The module
  replaces both lists each time they arrive (after every profile change),
  and clears them on stop and on disconnect.
* **Modes:**
  * `usb`, `lsb`, `am`, `nfm`, `wfm` and `cw` by their canonical names
    (`radioModeFromName`);
  * `sam` as AM and `bcfm` as WFM;
  * a digimode bookmark by its `underlying` mode, else the first
    underlying mode in `modes`;
  * anything else (DMR, YSF, DRM, DAB, ...) keeps mode −1: it tunes
    only, with the description saying what it is.
* **Bandwidth** from the `bandpass` of that mode in the `modes` message,
  `high_cut − low_cut`. Dials get the underlying mode's bandpass: USB,
  2.6 kHz.
* **Names.** Bookmarks keep the server's name. A dial is named by its mode
  in capitals ("FT8"), and its description is "dial frequency".
* **Frequencies** are stored as the server sends them, hardware Hz. The
  frequency manager subtracts the current tuning offset when it draws and
  tunes, so a converter offset changed later still lines up.

## Tests

* `owrx_client_tests`: the two messages parsed, including a digimode with
  `underlying`, a mode with no counterpart, and an empty list.
* The source's mode mapping is a pure function, tested on its own.

## Review gate (user tests)

* On an HF profile, the dials list shows FT8/FT4/WSPR labels at the right
  frequencies, and clicking one tunes the VFO in USB with about 2.6 kHz.
* The owner's bookmarks appear when that list is switched on.
* A profile change replaces both lists, and stopping the source removes
  them.
* "Copy to list..." saves an entry into a user list, and the source lists
  never appear in the frequency manager's saved config.
