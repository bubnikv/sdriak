# Plan: a remote-receiver browser for all networked sources

Date: 2026-11-25. Status: current. Steps 1–7 implemented, not committed:
built on Windows (bench tree), `remote_directory_tests` passes and the
parsers read the real documents of 2026-11-25; the GUI, live probes and
Connect are untested. Step 8 is open.

**Idea.** One browser for every networked source, grown out of the
KiwiSDR map selector of the SDRPPBrown port:

* **Directory providers** supply receivers;
* **each source module** supplies a prober and a connect handler;
* **one map + list UI** in core shows them all, with filters, testing and
  connecting.

## Why now

* **Six public directories serve the sources we have:**

  | directory | source | endpoint | entries | position | capacity | other fields |
  |-----------|--------|----------|---------|----------|----------|--------------|
  | KiwiSDR | `kiwisdr_source` | `http://rx.linkfanel.net/kiwisdr_com.js` (mirror of kiwisdr.com/public; no TLS) | 842 | `gps` "(lat, lon)" | `users` / `users_max` | `bands`, `snr`, `sdr_hw`, `ext_api`, `antenna`, `sw_version`, `offline` |
  | sdr-list.xyz | `kiwisdr_source` (PhantomSDR-Plus through its KiwiSDR bridge) | `https://sdr-list.xyz/api/v1/receivers`, a JSON array | 113 entries: 77 UberSDR (the instances.ubersdr.org set), 15 PhantomSDR+, ~20 others with no source | `lat_jittered`/`lon_jittered`, `grid_locator` | `users` / `max_users` | `software`, `version`, `range_start_hz`/`range_end_hz`, `last_seen`; no e-mail |
  | Web-888 | `kiwisdr_source` | `https://www.rx-888.com/api/devices`, `{devices[]}`, ~50 kB | 190 entries, 162 distinct, 89 `active` | `gps` "(lat, lon)" | `users` / `max_users` | `bands` (up to 64 MHz), `snr`, `antenna`, `status` (`active`, `Inaccessible`, `offline`), `uptime`; no e-mail. Almost none are in kiwisdr.com or Receiverbook |
  | UberSDR | `ubersdr_source` | `https://instances.ubersdr.org/api/instances`, `{count, instances[]}`, ~780 kB | 77 | `latitude`/`longitude`, `maidenhead` | `available_clients` / `max_clients` | `host`, `port`, `tls` (74 of 77), `public_url`, `public_iq_modes`, `tuning_range.{min,max}_frequency` (Hz), `snr_0_30_mhz`, `is_online`, `last_report_age_seconds`, `load_status`, `cw_skimmer`, `version` |
  | SpyServer | `spyserver_source` | `https://airspy.com/directory/status.json`, `{servers[]}` | 220 | `antennaLocation.{lat,long}` | `currentClientCount` / `maxClients` | `deviceType`, `minimumFrequency`/`maximumFrequency`, `maximumIQSampleRate`, `fullControlAllowed`, `streamingHost`/`streamingPort`, `online`, `lastSeen`, `generalDescription`, `antennaType`, `maxSessionDuration` |
  | Receiverbook | `openwebrx_source` | `https://www.receiverbook.de/map`, HTML with a `var receivers = [...];` JSON literal, ~380 kB | 1234 sites: 430 OpenWebRX, 721 KiwiSDR, 99 WebSDR | GeoJSON `location.coordinates` [lon, lat] per site | none | per receiver `{label, url, type, version}`; 205 OpenWebRX on 1.2.129 or later, 159 over https |

  Counts from 2026-11-25 (KiwiSDR from 2026-11-24).

  Web-888 receivers run KiwiSDR-derived firmware (`sw_version`
  `Web888_v2026.205`, `version_maj=2026`). The rx-888.com list's
  `active` status is not current: of three `active` receivers tried on
  2026-11-25, one timed out and one had no backend behind
  `proxy.rx-888.com`.

* **The KiwiSDR map selector was KiwiSDR-only,** while its `GeoMap` (pan,
  zoom, hit-test, tooltips) was generic. Two users shared it:
  `kiwisdr_source` (pick a server) and `websdr_view` (add a receiver to
  its list).
* **Every source with a directory exists and has a client usable from a
  worker thread:** `KiwiSDRClient` (core, http only), `UberSDRClient`
  (`admit()` is the blocking `POST /connection`, then IQ over wss),
  `spyserver::connect` + `waitForDevInfo` + `canControl()`, and
  `OpenWebRxClient` (handshake, `versionSupportsIq`). libcurl gives every
  HTTP fetch TLS and the proxy setting.
* **KiwiSDR TLS hosts** are still out of reach: the Kiwi client takes
  `host:port` over plain http
  ([kiwisdr-streams.md](../research/kiwisdr-streams.md) §7).

## Design

### Files (`core/src/gui/remote/`)

| file | content |
|------|---------|
| `remote_entry.{h,cpp}` | model, target parsing, JSON, Maidenhead, distance |
| `directory.{h,cpp}` | provider interface, `fetchDocument`, `DirectoryService` |
| `providers.{h,cpp}` | KiwiSDR, UberSDR, SpyServer, Receiverbook parsers and providers |
| `registry.{h,cpp}` | `RemoteHandler`, `ProbeResult`, the probe runner, `sigpath::remoteRegistry` |
| `recent.{h,cpp}` | recent and favourites in core config |
| `tcp_probe.{h,cpp}` | `probeTcp` for plain TCP servers |
| `browser.{h,cpp}` | `RemoteBrowser`, the shared browser, `connectEntry`, `shutdown` |
| `geomap.{h,cpp}`, `earcut.hpp` | the map widget |

Tests: `core/tests/remote_directory_tests.cpp`.

### Model

```cpp
enum class RemoteKind { KiwiSDR, UberSDR, SpyServer, OpenWebRX, SDRIAKServer, RtlTcp, NetSDR, Hermes, Other };

struct RemoteTarget {          // what a source needs to connect
    std::string scheme;        // "http", "https", "tcp"
    std::string host;
    int         port = 0;
    std::string path;          // optional
    std::string url() const;   // scheme://host:port/path
};

struct RemoteEntry {
    std::string id;            // stable: provider + host:port (or directory id)
    RemoteKind  kind;
    std::string provider;      // "kiwisdr.com", "ubersdr.org", "airspy.com", "receiverbook.de", "recent", "lan"
    std::string name, location;
    std::optional<std::pair<double, double>> latLon;  // degrees; absent: list view only
    RemoteTarget target;
    std::optional<int> users, usersMax;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;   // served frequency ranges, Hz
    std::optional<float> snrDb;
    std::optional<int64_t> lastSeenUnix;
    std::string version;       // server software version, when listed
    bool tlsRequired = false;
    bool favourite = false;
    std::vector<std::pair<std::string, std::string>> details;  // shown in the panel, already formatted
    std::vector<std::string> flags;        // "full-control", "wide-iq", "ext-api", "waterfall", "audio-only"...
};
```

* **Personal data.** The SpyServer and KiwiSDR directories publish owner
  e-mail addresses. Providers **drop e-mail fields at parse time**; they
  are never shown or cached.
* **One host, two protocols.** When one host appears in two directories
  (3 UberSDRs are also in the KiwiSDR list; Receiverbook repeats most
  KiwiSDRs), the browser shows one marker with every connect option,
  the native one first.

### Providers

* **The interface:**

  ```cpp
  struct FetchRequest {
      std::string etag, lastModified;      // from the cache, for a conditional GET
      std::function<bool()> shouldCancel;
  };
  struct FetchResult {
      bool notModified = false;            // 304: keep the cached entries
      std::vector<RemoteEntry> entries;
      std::string etag, lastModified;
  };
  struct DirectoryProvider {
      virtual std::string id() const = 0;  // cache file name, e.g. "kiwisdr.com"
      virtual std::string name() const = 0;
      // Worker thread. Gentle: one request, a timeout, honours the proxy setting.
      virtual FetchResult fetch(const FetchRequest& request) = 0;
  };
  ```

  `CurlResponse` gains `etag` and `lastModified`, read with
  `curl_easy_header` as `websock.cpp` reads `Location`.

* **The directory service** (one per process, shared by every browser
  view):
  * It starts when a browser first opens, never at startup.
  * It runs fetches on one background thread, one provider at a time.
  * It caches each provider's parsed entries on disk, with the fetch
    time and validators, in `core::args["root"]/remote_cache/<id>.json`,
    so the map opens instantly and offline. The cache holds parsed
    entries, so it never holds the dropped e-mail fields.
  * It refetches only when the cache is older than 15 min or the user
    presses Refresh. A failed fetch keeps the stale entries and shows the
    error next to the provider.
  * `remote::shutdown()` stops it, cancelling any transfer through
    `shouldCancel`, before the module manager unloads modules.
* **Built-in providers:**
  * **KiwiSDR:** the current parser, moved. `offline != "no"` entries are
    dropped, as now.
  * **Web-888:** `status == "active"` entries only, repeated receivers
    once. Each device is mapped onto the kiwisdr.com fields (`max_users`
    to `users_max`, `loc` from the part of the name after the last
    " | ") and goes through the KiwiSDR parser as KiwiSDR kind. When
    kiwisdr.com lists the same host:port, its entry wins.
  * **UberSDR:** `is_online` entries only; target from `host`, `port`,
    `tls`, falling back to `public_url`.
  * **SpyServer:** `online` entries only; target
    `tcp://streamingHost:streamingPort`. `fullControlAllowed` is shown as
    "allowed by the owner", not as a promise (see Open questions).
  * **Receiverbook:**
    * no API: the provider cuts the `var receivers = [...];` literal out
      of `/map` and parses it as JSON;
    * each site has a GeoJSON position and receivers with
      `{label, url, type, version}`, `type` being `OpenWebRX`, `KiwiSDR`
      or `WebSDR` ([openwebrx.md](../research/openwebrx.md) §1);
    * it offers OpenWebRX entries; those older than 1.2.129 (no `iq`
      mode) are kept, flagged `audio-only` and greyed, with the reason;
    * its KiwiSDR entries are emitted as KiwiSDR kind and merge with the
      KiwiSDR provider's entries by host:port (step 6);
    * WebSDR entries are skipped: we have no WebSDR source.
* **Recent and favourites.** A store backed by core config
  (`remoteReceivers.recent`, `remoteReceivers.favourites`), holding
  every target the user connected to or saved, for any kind. It needs no
  fetch. A source module reports a connection made outside the browser
  with `remote::recordRecent(entry)`. It covers `sdriak_server_source`,
  `rtl_tcp_source`, `rfspace_source` and `hermes_source`, which have no
  public directory. The per-module "Recent..." combos stay.
* **LAN discovery (optional, later).** HPSDR discovery broadcast, NetSDR
  UDP discovery, mDNS for SDRIAK servers. List-only; there are no
  coordinates.

### Registration (no core → module dependency)

A source module registers what only it can do:

```cpp
struct ProbeResult {
    bool ok = false;
    std::string reason;          // "too_busy", "badp", "TLS required", "refused", ...
    std::optional<int> rttMs;
    std::vector<std::pair<std::string, std::string>> negotiated;   // rates, modes, control
};

struct RemoteHandler {
    RemoteKind kind;
    std::string sourceName;                                  // as registered with SourceManager
    bool supportsTls = true;                                 // false: TLS entries are greyed
    std::function<ProbeResult(const RemoteEntry&, const std::atomic<bool>& cancel)> probe;   // worker thread
    std::function<void(const RemoteEntry&)> connect;         // UI thread
};
sigpath::remoteRegistry.registerHandler(handler);            // in the module constructor
sigpath::remoteRegistry.unregisterHandler(kind);             // in the destructor
```

* **What a probe does,** per protocol:
  * **KiwiSDR:** SND until the first IQ packet, tuned to the middle of the
    served range (the current tester, moved into `kiwisdr_source`).
  * **UberSDR:** `POST /connection`, then the first `iq48` packet; report
    `allowed_iq_modes` and the session limits.
  * **SpyServer:** handshake until `DEVICE_INFO` and the first
    `CLIENT_SYNC`; report `canControl`, the device and its maximum rate.
  * **OpenWebRX+:** the websocket handshake until the first `config`;
    report the version, whether it has the `iq` mode, the profile and
    the client count.
  * **Others** (`sdriak_server`, `rtl_tcp`, `rfspace`): TCP connect plus
    the first bytes the server sends; report the round trip.
* **Probe policy.** One probe runner in core (`Registry::startProbe`,
  state in `probeState()`), so probes run one at a time across every
  browser view and can be cancelled. `unregisterHandler`
  cancels and waits for a probe of that kind, so a module is never
  unloaded under a running probe.
* **What Connect does,** on the UI thread:
  1. stop the radio if it is playing;
  2. select the handler's source through the Source menu
     (`sourcemenu::selectSource`, exported), so its combo and the
     `source` config key follow;
  3. `handler.connect(entry)`: the module fills its host/port or URL,
     saves its config and its recent list, and connects as its own
     Connect or Play button would. KiwiSDR, UberSDR and OpenWebRX+ start
     playing; SpyServer and SDRIAK Server open the connection and leave
     Play to the user, as their menus do;
  4. record the entry in Recent.
* **Kinds with no handler** (module not loaded or not built) stay listed
  but greyed, with the reason.

### UI (`RemoteBrowser`, replacing `KiwiSDRMapSelector`)

* **Views.** A `RemoteBrowser` is a view over the shared directory
  service with its own map position, filters and selection, saved under
  its config prefix. Its action is Connect by default; a user can
  replace it (`websdr_view` uses "Add").
* **Map view.** `GeoMap` with one marker shape per kind (KiwiSDR ●,
  UberSDR ◆, SpyServer ▲, OpenWebRX+ ■, others ⬢), fill by state: free,
  nearly full (≥ 75 %), full, needs TLS the source lacks, not
  connectable (no handler, audio-only). Keep the current behaviour:
  cluster picker, phone-landscape footer, hide-full toggle, EXT API
  filter for KiwiSDR.
* **List view.** A sortable table: name, kind, location, distance from
  the user's QTH, users, ranges, SNR, last seen; with search. On phones
  the list is often the better view. Entries without a position (Recent,
  LAN) appear only here.
* **QTH.** No QTH setting exists yet. The browser keeps a Maidenhead
  locator field of its own in core config (`remoteBrowser.qth`); a later
  station-settings page can take it over.
* **Filters:**
  * kind;
  * "covers the current frequency" (uses `ranges` and
    `gui::freqSelect.frequency`);
  * free slots;
  * hide TLS entries a source cannot connect to;
  * full control (SpyServer);
  * IQ capable (hides audio-only OpenWebRX);
  * waterfall available;
  * favourites only.
* **Panel.** The selected entry's `details`, plus **Test**, **Connect**
  and **Favourite**, one Connect per protocol when a host has several.
* **Entry points:**
  * **Source menu:** a "Browse remote receivers..." button opens the
    shared browser with every kind. It is drawn from the main window, so
    it stays open when the menu column scrolls or collapses.
  * **Per source:** the "Browse..." button of `kiwisdr_source`,
    `ubersdr_source`, `spyserver_source` and `openwebrx_source` opens the
    shared browser filtered to its kind; that replaces the Kiwi map
    button.
  * **`websdr_view`:** its own view, filtered to KiwiSDR, with "Add".

### Gentleness

* **Directory fetches:** one request per provider per refresh, sequential,
  cached, conditional where the server sends validators.
* **Probes:** user-initiated, one at a time, short, closed cleanly.
* **No background sweeps.** Nothing probes many receivers unattended. A
  "test all visible" action, if ever added, is sequential with pauses and
  capped.
* **One network layer.** Everything goes through libcurl and the same
  proxy setting, so the browser and the sources behave the same behind a
  proxy.

## Steps

Steps 1–7, 9 and the Web-888 provider are implemented as described; step 8 is open.

1. **Extract, behaviour-neutral.** Move `GeoMap` and `earcut.hpp` to
   `core/src/gui/remote/`; add the model, the directory service with its
   cache, and a `KiwiSDRProvider` with the moved parser; build
   `RemoteBrowser` from the map selector's markers, cluster picker and
   panel. `kiwisdr_source` and `websdr_view` keep working, filtered to
   KiwiSDR.
2. **Registry and handoff.** Add `RemoteHandler`, the probe runner and the
   Connect flow; the Source-menu entry point. Move `KiwiSDRTester` into
   `kiwisdr_source` as its probe; register its connect.
3. **SpyServer provider and handler.** 220 receivers. The probe shows
   `canControl`, because many servers are shared.
4. **UberSDR provider and handler.** 77 receivers, nearly all over TLS,
   which `UberSDRClient` handles.
5. **Receiverbook provider and OpenWebRX+ handler.** 430 receivers, 205
   with IQ; older ones greyed as audio-only.
6. **List view, filters and favourites/recent** for every kind, including
   handlers for `sdriak_server_source`, `rtl_tcp_source` (probe: the
   12-byte `RTL0` header) and `rfspace_source`, which have no directory.
   These three register only in the GUI, not on a headless server.
7. **De-duplication across directories** and native-first connect
   options.
8. **Optional:** LAN discovery providers, and `hermes_source` through
   them.
9. **sdr-list.xyz provider** (`GET https://sdr-list.xyz/api/v1/receivers`,
   [phantomsdr-family.md](../research/phantomsdr-family.md) §2). It emits
   only `software == "PhantomSDR+"` entries, as KiwiSDR kind, for the
   KiwiSDR bridge (§5); the UberSDR entries are the instances.ubersdr.org
   set, and NovaSDR, FernSDR and the rest have no source. Entries not seen
   for a day are dropped. The KiwiSDR probe tells IQ from audio-only (the
   operator's `[server] iq`). Port 443 entries are https, which the
   KiwiSDR handler greys. The bridge labels every waterfall line zoom 0,
   so `kiwisdr_source` detects a server whose headers never follow a zoom
   request and keeps that session at zoom 0.

## Open questions

* **The "covers the current frequency" filter.** With no source running
  it uses the last tuned frequency the frequency selector still holds.
* **SpyServer `fullControlAllowed`.** It is true on nearly every server,
  yet `canControl` depends on who else is connected. Show it from the
  probe, not from the directory.
* **Receiverbook's terms.** It publishes no API; one fetch per 15 min of
  a public page is the gentle use. Ask the operator if usage grows.
