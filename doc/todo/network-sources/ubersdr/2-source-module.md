# UberSDR 2: source module

Date: 2026-11-24. Status: implemented and built in the bench tree, not
committed; not run against a server or in the GUI. Where the code differs
from this plan:

* **Admission thread:** `admit()` and `UberSDRClient::start()` run on a
  worker thread started by `start()`, because admission is an HTTPS
  request.
* **Width fallback:** when the chosen width is not in `allowed_iq_modes`,
  the session takes the widest allowed one below it.
* **Width change:** changing the IQ width while playing calls
  `UberSDRClient::start()` again with the same session id; the server
  replaces the primary.
* **Server field:** a URL or `host[:port]`, https by default. The recent
  list is up to 8 entries with no delete button. There is no session id
  in config: a new one is made each run.
* **Link-rate guard:** it runs on the network thread. The downgrade button
  offers the next narrower width.
* **Rate:** the DSP runs at the packet header's rate, applied from the UI
  tick (`uiTickHandler`).

Part of [README.md](README.md).
Prerequisite: UberSDR 1.

## Files

* `source_modules/ubersdr_source/CMakeLists.txt`: uses
  `include(${SDRPP_MODULE_CMAKE})`.
* `source_modules/ubersdr_source/src/main.cpp`.
* Root `CMakeLists.txt`: add `OPT_BUILD_UBERSDR_SOURCE` (default ON) and
  the module directory.
* `core/src/core.cpp`: add `ubersdr_source.so` to the Android module list
  and the default module list.

## Module

* **Registration.** `sigpath::sourceManager.registerSource("UberSDR", &handler)`,
  with handlers for menu, select, deselect, start, stop, tune and
  `uiTickHandler`. One instance.
* **Pipeline.** The client's `onIQ` writes into a `rawStream`, which feeds
  a `PrebufferedLink`, which feeds `handler.stream`, as `kiwisdr_source`
  does. Default target 150 ms; slider 0–1000 ms. Count samples pushed, for
  markers (core 2).
* **Menu:**
  * server: `host:port` or a URL, plus a recent-servers list, as in
    `kiwisdr_source`. A "Browse…" button comes later with the remote
    browser.
  * optional password (stored in module config; say it is plain text);
  * **IQ width** combo, filled from `allowed_iq_modes` after admission;
    default `iq48`;
  * a **server waterfall** checkbox (UberSDR 3);
  * prebuffer slider;
  * status: admitted or refused (reason), session time left (when
    `max_session_time > 0`), measured link rate, `baseband_power`.
* **Start:**
  1. `admit()`;
  2. `openIQ(mode, C_hw)`;
  3. `core::setInputSampleRate(rate)`, where the rate comes from the
     packet header and equals the mode's nominal rate (48 000, 96 000,
     192 000, 384 000);
  4. register the provider (UberSDR 3).
* **Changing the IQ width** reconnects the primary: the server replaces
  the old one.
* **Stop:** close the sockets, unregister the provider, `clearIQWindow`,
  `clearDisplaySpan`.
* **`tune(freq)`** moves C. With the window policy active (UberSDR 4) the
  IQ centre follows the policy; otherwise `tune` re-centres the IQ on C,
  as a plain wide-IQ source would.

## Link-rate guard

* **Why.** Over a mobile link, `iq96` (285 kB/s) kept up but `iq192`
  (517 kB/s) fell 7 % behind. A wide mode the link cannot carry turns into
  ever-growing latency and prebuffer drops.
* **The guard.** Every 5 s compare the sample arrival rate with the
  header rate. Below 98 % for two windows in a row, show
  "Link too slow for <mode>" with a one-click downgrade.
* **Default:** `iq48`.

## Review gate (user tests)

* **Instances:** three directory instances, chosen as in ubersdr.md §4:
  online, TLS, free capacity.
* **IQ:** clean audio in USB/AM on a broadcast carrier.
* **Wide IQ:** `iq96` on an instance that allows it.
* **Refusal:** a wrong password shows the reason.
* **Android:** the module loads.
