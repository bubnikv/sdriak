# Plan: a remote-receiver browser for all networked sources

Date: 2026-11-25. Status: current. Steps 1–7 implemented, not committed:
built on Windows (bench tree), `remote_directory_tests` passes and the
parsers read the real documents of 2026-11-25; the GUI, live probes and
Connect are untested. Step 8 is open. Steps 10–15 (receiver quality, the
tabular view for desktop and phone) are planned, not built.

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
  the list is often the better view. Its planned redesign is under
  [Tabular presentation](#tabular-presentation). Entries without a position (Recent,
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

### How other directories present receivers

Surveyed 2026-11-25 from each site's own page code and data feed.
kiwisdr.com/public sits behind a click-through and sdr-list.xyz is a
client-rendered app, so neither showed its controls.

| directory | layout | per receiver | filters and sort | quality |
|-----------|--------|--------------|------------------|---------|
| instances.ubersdr.org | map + sidebar, compact or expanded cards | online dot, flag, callsign, free/max, name, day/night icon, location, km, PSKReporter rank, band badges | search, country, max distance, min free slots, min SNR, min session, IQ width, range (≤ 30 MHz / above), online, antenna, unlimited session, day only / night only, 9 features, condition per band (Any / Fair+ / Good+ / Excellent); sort distance, listeners, SNR | FT8 SNR per band, spectrum SNR pair, noise floor per band |
| rx-888.com (Web-888) | map + cards, summary tiles | name, region, mode, "SNR All / HF", free slots, listeners, antenna, uptime | search, status, region (continent), coverage (HF / VHF); sort "Best SNR" (default) or "Most free slots" | the KiwiSDR pair |
| rx.linkfanel.net (dyatlov) | map; a separate SNR ranking page | antenna, users, "S/N score", "Blocked spectrum %" | presets in the URL fragment (`#gps`, `#all`, `#62`, `#20khz`); by default only receivers wider than 5 MHz | marker colour = SNR on 0–20 dB; a rank score |
| airspy.com (SpyServer) | globe + cards | listeners x/y, device, current frequency, description | search; All / Available / Full / Unreachable | none |
| websdr.org | table: "Location and URL", "Frequency range", "Antenna", band cells coloured by activity | as the columns | band (LF/MF, 160m … UHF), region, mobile support, covering a given MHz | users per band |
| receiverbook.de | paged cards + map | site, link, software and version, band tags | receiver type; frequency, band or service | none |
| ab9il.net | plain link lists | name | none (browser find) | KiwiSDR top 75 by SNR, low SNR excluded |

What they share:

* **4–8 fields per receiver.** The rest is in a popup or card.
* **Free slots and distance** are both columns and filters.
* **A coarse region filter** (continent or country) sits next to distance.
* **Quality is shown with its measurement time,** and stale or missing
  values sort last.

### Receiver quality

* **KiwiSDR's SNR** (`SNR_calc` in Beagle_SDR_GPS `rx/rx_util.cpp`):
  * **How it is measured:** hourly by default (the owner can choose 0, 1,
    4, 6 or 24 h), an internal client averages 32 frames of the full-span
    1024-bin waterfall.
  * **The formula:** SNR = 95th percentile minus median, in dB.
  * **The directory value:** `snr` is `"all,HF"`, over 0–30 MHz and
    1.8–30 MHz.
  * **Antenna check:** an HF value of 3 dB or less, with at most two bins
    above −100 dBm, sets `ant_connected=0`.
  * **Masked bins** (−190 dB) are skipped. rx.linkfanel.net publishes the
    masked share as "blocked spectrum" and scales its rank score by it.
  * **History:** each Kiwi keeps a week of hourly values, with 0–2,
    2–10, 10–20 and 20–30 MHz splits, at `/snr`.
  * **Web-888** uses the same `snr` pair.
  * **What it measures:** contrast, not sensitivity. Strong MW
    broadcasters raise the 0–30 MHz value and a noisy site lowers it. It
    is higher at night, and linkfanel calls it "most relevant when
    comparing receivers located in the same area". dyatlov prefers the
    HF value and falls back to "all" only when HF is missing.
* **UberSDR** publishes more with `GET /api/instances?conditions=true`.
  That is the URL its own page uses, and it adds 30 kB to the plain
  ~510 kB:
  * `band_conditions`: SNR in the FT8 segment of each band, 2200 m to
    6 m;
  * `avg_band_snr`: their mean without 6 m, the directory's sort key;
  * `conditions_updated_at`: older than 30 min counts as stale;
  * `snr_0_30_mhz`, `snr_1_8_30_mhz`: the KiwiSDR-style pair;
  * `antenna_connected`, `pskreporter_rank`, `country_code`.

  Its grades are POOR < 6, FAIR 6–20, GOOD 20–30, EXCELLENT > 30 dB. On
  2026-11-25 the correlation between `snr_1_8_30_mhz` and
  `avg_band_snr` over 76 instances was 0.25: the spectrum value says
  little about what is heard on a given band.
* **Other KiwiSDR fields that bear on quality:** `adc_ov` (ADC overload
  count), `gps_good` and `fixes` (TDoA-grade clock), `preempt`, `uptime`
  and `updated`.
* **SpyServer and Receiverbook publish no quality value.**
* **The measure that applies to every kind** is the same statistic
  computed by us: P95 minus median of the spectrum.
  * **Why it works everywhere:** it is a difference in dB, so it needs no
    calibration. SpyServer reports dBFS and OpenWebRX is uncalibrated.
  * **The catch:** it depends on the span. A 2 MHz SpyServer at VHF and a
    30 MHz Kiwi are not comparable over their full spans.
  * **The fix:** compute it over one window, the band around the current
    frequency, or ±100 kHz outside the ham bands. The value then compares
    receivers for what the user wants to hear.
  * **Which spectrum each probe reads:** KiwiSDR W/F frames, the UberSDR
    spectrum, the SpyServer FFT stream, the OpenWebRX waterfall. The
    SpyServer probe now stops at `CLIENT_SYNC` and the OpenWebRX probe at
    `config`, so both must read a few frames more (about 2 s).
  * **The limit:** it is probe-only. Gentleness forbids sweeping the
    directory, so it ranks the receivers the user tests, not all of them.
* **Absolute noise floor (dBm/Hz)** is calibrated only on KiwiSDR and
  UberSDR: a detail, not a ranking key.
* **Decode-based measures** (FT8 SNR, PSKReporter, WSPRnet or RBN spot
  counts) would fit any receiver that spots under a callsign. Only
  UberSDR links them today, and matching other receivers by callsign is
  fragile: not pursued.

**The model gains:**

```cpp
enum class QualitySource { None, SpectrumAll, SpectrumHF, Ft8Bands, Probe };

struct RemoteQuality {
    float db = 0.0f;
    QualitySource source = QualitySource::None;
    std::optional<int64_t> measuredUnix;           // absent: directory gives no time
    std::vector<std::pair<std::string, float>> bands;   // "40m" -> dB, FT8 or probe
};
// in RemoteEntry, replacing snrDb:
RemoteQuality quality;
std::optional<bool> antennaConnected;
bool adcOverload = false;
std::string countryCode, countryName, continent;   // from the position, see below
```

* **Which value each provider fills:**
  * **KiwiSDR, Web-888:** the HF value (`SpectrumHF`), the "all" value
    when HF is 0 (a transverter offset), timestamp `updated`;
    `ant_connected`; `adc_ov`.
  * **UberSDR:** `avg_band_snr` and `band_conditions` (`Ft8Bands`) while
    `conditions_updated_at` is under 30 min old, otherwise
    `snr_1_8_30_mhz` (`SpectrumHF`); `antenna_connected`.
  * **sdr-list.xyz, SpyServer, Receiverbook:** none until probed.
* **A probe result** sets a `Probe` quality on the entry for the session,
  with the window it measured, and wins over the directory value.
* **Grades.** FT8 and probe values use UberSDR's thresholds (6 / 20 /
  30 dB). Spectrum values run higher (KiwiSDR fleet median about 25 dB,
  top about 45 dB), so they are graded by percentile within their own
  source: top 25 % excellent, next 25 % good, next 35 % fair, the rest
  poor.
* **The rank key** for sorting across kinds is the percentile within the
  source, 0–1. Entries with no value rank as 0.4: below good, above poor.
  Stale values (FT8 over 30 min old, spectrum over 6 h old) drop one grade
  and show their age.
* **Country and continent** come from the position by point-in-polygon
  against `res/cty/map.json`, which the map already loads. Its 175
  Natural Earth features carry `iso_a2`, `name` and `continent`. The
  lookup runs once per entry at rebuild; a point in no polygon (coast,
  small island) takes the nearest polygon within 50 km, else stays
  blank. UberSDR's own `country_code` wins when present.
* **Day or night at the receiver** is the solar elevation at its
  position, computed at draw time.

### Tabular presentation

The list view answers "find a receiver by quality, availability and
location" better than the map: the map shows where receivers are, but it
cannot rank them. The list becomes the default view, and the map stays
one click away.

* **One row source.** The filtered, sorted row vector the table draws
  (`m_rows`) also feeds the map. The map shows the same filtered set, and
  selection is shared, so a row picked in the list is centred on the map.
* **Filter bar.** The filters people use most move out of the popup into
  one row above the table:
  * **Quality:** Any / Fair+ / Good+ / Excellent;
  * **Available:** hides full receivers, receivers with no handler and
    TLS entries the source cannot open (today's "Hide full" and
    "Connectable only" merged);
  * **Distance:** Any / 500 / 1000 / 2000 / 5000 km. It is disabled with
    a hint when no QTH is set;
  * **Region:** Any / one continent / one country, a combo built from the
    countries present;
  * **Here:** "covers the current frequency";
  * **Search,** as now.

  The other filters stay in "More filters...": kinds, full control, IQ,
  waterfall, EXT API, favourites, antenna connected, day / night at the
  receiver. A badge on the button counts the active ones.
* **Sort presets** in a combo, next to clickable headers:
  * **Best** (the default): grade first, then free slots, then distance;
  * **Nearest;**
  * **Most free;**
  * **Quality:** the rank key.

  A click on a header overrides the preset until the preset is chosen
  again.
* **Desktop columns** (wide: 70 em or more):

  | column | content | sort |
  |--------|---------|------|
  | ★ | favourite toggle, clickable | favourites first |
  | Name | kind glyph (the map's marker shape and fill), then the name; greyed with the reason in a tooltip when not connectable | name |
  | Quality | grade chip, value, source letter (S spectrum, F FT8, P probe); tooltip with per-band values and age | rank key |
  | Users | free/max as a small bar, red when full | free slots |
  | km | distance from the QTH | distance |
  | Country | two-letter code, full name in a tooltip | name |
  | Range | served range in MHz | upper edge |
  | Seen | age | age |

  These are 8 columns. Hidden by default and enabled from the header
  context menu (`ImGuiTableFlags_Hideable`, already set): Kind,
  Location, Antenna, Version, Bands (the FT8 band badges, as UberSDR
  shows them).
* **Medium width** (40–70 em: a small desktop window, a tablet, a phone
  in landscape). The same table drops columns by priority: Range, Seen,
  Country, then km. Name, Quality and Users always stay. A column the
  user enabled by hand is kept.
* **Phone** (under 40 em, the portrait phone):
  * **No table.** Each receiver is a two-line row with a full-width touch
    target, at least `style::touchStyle` height:

    ```
    ▲ OK1XYZ Praha 40m loop                     GOOD 28 S
      CZ · 312 km · 3 of 8 free · 5 min ago
    ```

  * **Controls.** Search on its own row; under it the Sort combo and a
    "Filters (n)" button.
  * **Filters page.** The button opens a full-screen filters page with
    large controls: the same filter state as desktop.
  * **Detail page.** A tap opens a full-screen detail page in place of
    the list: all `details`, the per-band chips, Test, Connect, Favourite,
    "Show on map", and Back. It replaces the 40 % bottom panel, which
    leaves too few rows on a phone.
  * **Map / List** stays a two-way toggle in the top row.
  * **Scrolling.** `ImGuiListClipper` with a fixed two-line row height,
    so 1,500 rows scroll smoothly.
* **The tier is chosen from the popup's content width in em,** not from
  the platform. A narrow desktop window gets the phone layout too, and
  the layout follows `style::touchStyle` rather than `__ANDROID__`.
* **Persistence.** The filter bar, the sort preset and the user's column
  choices are saved under the view's config prefix, as the filters are
  now. The phone and desktop tiers share the filter state, not the
  column state.

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

Steps 1–7, 9 and the Web-888 provider are implemented as described; steps
8 and 10–15 are open.

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
10. **Quality model.** `RemoteQuality` replaces `snrDb`. KiwiSDR and
    Web-888 take the HF value; the UberSDR provider fetches
    `?conditions=true` and fills `Ft8Bands`; `antennaConnected` and
    `adcOverload` are filled. Per-source percentile grades and the rank
    key are computed in `rebuild()`. Tests: parsing the pair with HF 0,
    stale FT8 data, grade boundaries.
11. **Country and continent.** Point-in-polygon against `map.json`,
    loaded through `GeoMap`'s parser but kept apart from drawing, so the
    list works before the map has ever drawn. Tests: a handful of known
    receivers, a coastal point, mid-ocean.
12. **Desktop table.** The new columns, sort presets, filter bar, "More
    filters..." with its count, and the list as the default view. One
    row vector feeds list and map, and selection is shared.
13. **Responsive tiers.** Width-driven column priority (medium), then
    the two-line phone rows with full-screen filter and detail pages.
    Test on Android portrait and landscape, and in a narrow desktop
    window.
14. **Probe quality.** Each handler's probe reads a few spectrum frames
    and reports P95 minus median over the window around the current
    frequency (`ProbeResult` gains a `std::optional<RemoteQuality>`).
    SpyServer and OpenWebRX+ probes are extended past their current stop
    points. The value shows as source P.
15. **Day / night filter and band chips:** solar elevation at the
    receiver, and the FT8 per-band chips in the Bands column and the
    detail page.

## Open questions

* **The "covers the current frequency" filter.** With no source running
  it uses the last tuned frequency the frequency selector still holds.
* **SpyServer `fullControlAllowed`.** It is true on nearly every server,
  yet `canControl` depends on who else is connected. Show it from the
  probe, not from the directory.
* **Receiverbook's terms.** It publishes no API; one fetch per 15 min of
  a public page is the gentle use. Ask the operator if usage grows.
