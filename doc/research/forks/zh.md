# lzh173/sdriak-zh — features worth porting

Review date: 2026-11-24.

**What was reviewed.** [`lzh173/sdriak-zh`](https://github.com/lzh173/sdriak-zh)
(BI7KGA), a Simplified-Chinese edition of SDRIAK, GPL-3.0, created
2026-11-08:

* `master` at [`bb8c3339`](https://github.com/lzh173/sdriak-zh/commit/bb8c3339)
  (2026-11-18);
* the branches `zh-cn` ([`4b882999`](https://github.com/lzh173/sdriak-zh/commit/4b882999))
  and `feat/file-seek` ([`ef791b39`](https://github.com/lzh173/sdriak-zh/commit/ef791b39)),
  both already merged into `master`.

Fetched as the `zh` remote:
`git remote add zh https://github.com/lzh173/sdriak-zh.git`.

## State of the fork

* **It tracks us.** `master` last merged our tree at `14c7f176`
  (2026-11-17, the cw_engine port, PR #3), and is 59 commits ahead and 46
  behind. Unlike jp, it merges our work regularly
  (`c4fab0b9`, `1e7e7ede`, `eca8b40e`), so its code is written against our
  current APIs: scoped config transactions, the post-split resampler, the
  DSP-pipeline changes.
* **The author already contributes.** lzh173 reported issue #26 and
  co-authored our `0ff0643f` (*deps: avoid Android startup block in Mbed
  TLS entropy init*). Their own commit for it, `497c8de6`, is the same fix.
* **Quality.** Commit messages give a root cause and a reasoned fix, in
  the same style as ours. Some fixes come with a debug-build harness
  (`4a92490a`, `b84c2d31` for FT8), but there are no automated tests.
* **Shape** (non-merge commits, lines changed):

  | area | lines | nature |
  |------|-------|--------|
  | `core/src/gui/cjk_glyphs.h` + Noto Sans SC subset | about 3 600 | generated glyph tables for the Chinese UI |
  | `decoder_modules/ft8_decoder` | 5 712 | its own module on vendored `ft8_lib` (MIT): 842 lines of module, the rest library |
  | Chinese strings across about 60 files | about 1 200 | English UI literals **replaced** by Chinese (`ba5404ce`) |
  | `core/src/utils/proxy.{h,cpp}`, `websock`, KiwiSDR client, directory, map, source | about 700 | **HTTP proxy support for every KiwiSDR connection** |
  | `misc_modules/china_ref` | 525 | Chinese frequency annotations, channel tables, repeater calculator |
  | `source_modules/file_source` | 339 | seekable playback bar |
  | Android `MainActivity.kt`, backend, recorder | about 450 | system document picker, recording export |
  | `core/src/utils/file_utils.h` and readers/writers | about 210 | **non-ASCII paths on Windows** |
  | build | small | spdlog bundled on distro profiles |

## Bottom line

| Change | Recommendation | Why |
|---|---|---|
| Non-ASCII paths on Windows (`c2c006bb`) | **Port** | Real bug of ours: narrow file APIs read `std::string` paths in the ANSI code page, so installing or recording under a non-ASCII directory fails to load fonts, icons, band plans and themes, and the first frame crashes. |
| `file_source` on Android + system document picker (`5a427829`, `4f5c3bd7`, `f4bad078`) | **Port** | Real bug of ours: `file_source.so` is missing from the hard-coded Android module list in `core.cpp`. The fix uses `ACTION_OPEN_DOCUMENT`, which is what [jp.md](jp.md) recommends in place of jp's "All files access". Recording export uses `ACTION_CREATE_DOCUMENT`. |
| spdlog bundled on distro profiles (`bb8c3339`) | **Port** | One line in `DepClassification.cmake`. Debian sid's system spdlog is built against libfmt12 and breaks the bundled `librfnm` link. |
| ImGui `_PathArcToFastEx` divide-by-zero guard (`8951e812`) | **Port** | A two-pixel rounded rect makes ImGui divide by a zero segment count (`0xC0000094`). The guard is in our vendored `imgui_draw.cpp`, and any widget can hit it. |
| HTTP proxy for KiwiSDR (`cb338f40`, `ad77b6f2`, `0b5967c7`) | **Port, with fixes** | Makes KiwiSDR usable behind proxies; the directory, `/VER`, the tester and the websocket all go through it. See §2 for the gaps and how it fits the TLS work. |
| Seekable `file_source` playback bar (`0d222494` …) | **Port** | Self-contained and useful. It already went through several fixes for thread safety, stale seeks and the ImGui crash. |
| FT8/FT4 decoder | **Prefer over jp's/F4JTV's as the base** | Written against our current tree, MIT `ft8_lib`, worker thread with mutex-guarded state, one instance, waterfall labels, automatic slot time-offset correction, properly disableable. No WSPR. Needs known-slot tests, as noted for jp's in [jp.md](jp.md) §3. |
| Simplified-Chinese UI | **Not as is; take the requirement** | It edits literals in place, so it is a separate product, not a translation layer. If we want localization, the font-subset and glyph-range generators (`scripts/`) are reusable; the string layer must be designed. |
| `china_ref` module | **Skip (region-specific)** | Chinese channel tables and band plan. The repeater/CTCSS calculator and the searchable annotation list overlap our EiBi and frequency-manager plans. |
| Fork identity, signing keys, release notes | **Skip** | Fork-specific. |

## 1. Worth porting

### 1.1 Non-ASCII paths (Windows)

[`c2c006bb`](https://github.com/lzh173/sdriak-zh/commit/c2c006bb) adds
`core/src/utils/file_utils.h`, which routes core file access through
`std::filesystem` (the wide Win32 API) and treats paths returned by the
native file dialogs as UTF-8. It applies this to:

* resource loading in `style.cpp`;
* `wav.cpp`, `flac_reader.cpp` and `riff.cpp`;
* the file source and the recorder;
* bookmark import and export;
* the meteor and radiosonde outputs.

The defect is ours: `core/src/utils/wav.cpp` still calls
`fopen(path.c_str(), …)`. Take the helper and audit for remaining narrow
`fopen`/`ifstream` uses on user-supplied paths. The commit lists the ones
it found.

### 1.2 Android: `file_source` and document picker

[`5a427829`](https://github.com/lzh173/sdriak-zh/commit/5a427829) and
[`4f5c3bd7`](https://github.com/lzh173/sdriak-zh/commit/4f5c3bd7):

* **The missing module.** `file_source.so` was compiled into the Android
  build but never loaded. Android's `dlopen` resolves only names from the
  native-library directory, and our `core.cpp` list (under
  `#ifdef __ANDROID__`) does not include it.
* **The picker.** `MainActivity.pickFileBlocking()` starts
  `ACTION_OPEN_DOCUMENT` and returns a readable path: the filesystem path
  for external-storage documents, otherwise a copy in the app cache. It is
  exposed as `backend::pickFile()` for the file-dialog worker. It probes
  by actually opening and reading, because `access(2)` can report a path
  readable while `open(2)` is denied.
* **Permission.** It asks for `READ_MEDIA_AUDIO` on Android 13+.
* **Export.** The recorder panel lists recordings and exports them
  through `ACTION_CREATE_DOCUMENT`.

This is the storage-access-framework route that [jp.md](jp.md) ("Earlier
features still present") asks for. The same bridge can serve bookmark
import and export.

### 1.3 Build and ImGui

* [`bb8c3339`](https://github.com/lzh173/sdriak-zh/commit/bb8c3339):
  `spdlog` → `distro:bundled`. spdlog is ABI-coupled to its `fmt`.
* [`8951e812`](https://github.com/lzh173/sdriak-zh/commit/8951e812):
  guard `_CalcCircleAutoSegmentCount(radius) == 0` in
  `ImDrawList::_PathArcToFastEx`. `PathRect()` clamps rounding to
  `size·0.5 − 1`, so a rect a little over 2 px wide leaves a radius of
  about 1e-6 and a segment count of 0.

### 1.4 Seekable playback

[`0d222494`](https://github.com/lzh173/sdriak-zh/commit/0d222494) through
[`012909d1`](https://github.com/lzh173/sdriak-zh/commit/012909d1): an
elapsed/total time bar in the File source that seeks while dragged. It
uses the readers' existing frame-granular seek. Follow-up fixes:

* the seek-bar reads moved off the playback thread (`8ca646e9`);
* a stale seek is dropped when playback starts (`6a92792c`);
* the clamp bounds are floats (`86843358`);
* an attempt to load the file before the first frame was reverted, and
  the load-on-menu timing kept (`012909d1`).

Take the end state, not the series.

### 1.5 FT8/FT4 decoder

[`0af8c01d`](https://github.com/lzh173/sdriak-zh/commit/0af8c01d) and 17
follow-ups on 2026-11-15:

* **Audio and timing.** It binds a radio's demodulated audio stream via
  `sinkManager.bindStream()`, resamples to 12 kHz, and feeds `ft8_lib`'s
  monitor in UTC-aligned slots (15 s FT8, 7.5 s FT4).
* **Threads.** Decoding runs on a worker thread; queue, decoder, results
  and history each have their own mutex. One instance.
* **Display.** Decodes are listed with SNR, dt and audio offset; clicking a
  row tunes to it, and the text is drawn on the waterfall.
* **Time correction.** Slot time-offset correction is manual or automatic.
  It integrates dt rather than jumping to it, hunts for the window when
  the audio is misaligned, and steers only with two or more decodes. There
  is also a "decode now" for the last full slot.
* **Platform fixes.** MSVC needed two changes in the vendored code: VLAs
  in `monitor.c`, and `stpcpy` in `message.c`.
* **Housekeeping.** It is properly disableable: it unbinds, hides the
  panel and stops the labels.

Compared with jp's module ([jp.md](jp.md) §3), it is smaller, already on
our APIs, free of jp's file-global hash-table race, and carries no WSPR
or FFTW. It is the better base. jp's `RR73`-is-not-a-locator fix and
QTH/distance display could be added on top. Before adoption, add
known-slot recordings with expected decodes, and label the SNR as
`ft8_lib`'s estimate.

## 2. KiwiSDR and networking

The only networking work is **HTTP proxy support for KiwiSDR**
([`cb338f40`](https://github.com/lzh173/sdriak-zh/commit/cb338f40),
[`ad77b6f2`](https://github.com/lzh173/sdriak-zh/commit/ad77b6f2),
[`0b5967c7`](https://github.com/lzh173/sdriak-zh/commit/0b5967c7)).

**The problem.** Our KiwiSDR path (directory fetch, `/VER`, websocket)
runs on raw sockets, so libcurl's proxy handling never applies, and
receivers are unreachable behind a mandatory proxy, which is common in
China and in corporate networks.

**What it adds:**

* `net::proxy::systemSettings()` reads the Windows Internet Settings
  (WinINET `ProxyEnable`, `ProxyServer` with per-scheme lists, and
  `ProxyOverride` as a bypass list with `<local>` and `*` globs). On other
  platforms it falls back to `http_proxy`/`all_proxy`.
* `net::proxy::connect()` opens a direct socket or an **HTTP CONNECT
  tunnel**, honouring the bypass list. `net::http`, the directory client
  and `KiwiSDRClient` use it, and `WSClient::setProxy()` makes the
  websocket handshake tunnel too.
* A desktop-only "use system proxy" checkbox (`kiwisdr_use_system_proxy`)
  and a manual `host:port` override (`kiwisdr_proxy_addr`). The override
  is there because a proxy can be configured while `ProxyEnable` is 0. A
  hint shows the proxy actually in effect.
* The map's receiver tester follows the same setting. The directory gained
  `invalidate()` and a retry button, so a list that failed before the
  proxy was enabled refetches.

**Gaps to fix when porting:**

1. **No proxy authentication.** Credentials in `user:pass@host` are
   stripped, and no `Proxy-Authorization` is sent, so proxies that require
   a login fail.
2. **IPv6.** The CONNECT authority is `host:port` without brackets for
   IPv6 literals; it should be `[v6]:port`.
3. **Environment variables.** `https_proxy` and `no_proxy` are not read.
   The bypass list comes only from WinINET, so on Linux/macOS every host
   goes through the proxy.
4. **Whitespace churn.** The `websock.cpp` diff rewrites the license
   header, which looks like line-ending churn. Port the logic, not the
   whitespace.
5. **One flag, two layers.** The global toggle lives in `net::proxy` but
   is persisted by the KiwiSDR module. If the UberSDR source or others use
   it, move the setting to core network settings.

**Fit with our own KiwiSDR plan**
([kiwisdr-streams.md](../kiwisdr-streams.md) §7, step 0):

* **TLS goes inside the tunnel.** TLS must run *inside* the CONNECT
  tunnel: open the tunnel, then start TLS to the target host, with SNI for
  the target, not the proxy.
* **Layering.** Put the TLS layer under `WSClient` so the proxy code, the
  TLS code and the websocket framing stack cleanly. The same applies to
  the planned native UberSDR source ([ubersdr.md](../ubersdr.md)): 74 of
  77 UberSDR instances need TLS, and the same users need the proxy.
* **What zh leaves unchanged.** It does not touch the KiwiSDR stream
  defects we measured: no TLS, flagged packets dropped by
  `msg[3] == 0x08`, the rate hard-coded at 12 000, the `/kiwi/` web-UI
  path.

## 3. Not porting

* **The Chinese UI.** It changes user-visible literals in about 60 files
  to Chinese (`ba5404ce`). Config keys, module names and menu order keys
  stay English, and menu titles are mapped at draw time. It works for a
  single-language fork, but it is not a translation layer, and merging it
  would make our UI Chinese.
  * **Reusable if we localize:** the Noto Sans SC subset generator and the
    CJK glyph-range table generator (`scripts/`, `core/src/gui/cjk_glyphs.h`).
  * **The real work** would be a string catalogue, i.e. gettext-style
    lookup at the literals.
* **`china_ref`.** Chinese channel tables (409 MHz public transceivers,
  marine, aviation, GSM-R), a Chinese band plan (`china-zh.json`) and a
  repeater/CTCSS calculator. Region-specific. The repeater calculator
  might be useful in general, but it is small and not urgent.
* **`2e4ef5a7`** dropped the `CW_ENGINE` entries while our CMakeLists
  listed `decoder_modules/cw_engine` before the directory was public. It
  is resolved upstream since `14c7f176`, which zh has merged.
