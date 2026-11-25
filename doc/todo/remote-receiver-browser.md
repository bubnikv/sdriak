# Plan: a remote-receiver browser for all networked sources

Date: 2026-11-24. Status: current. Nothing implemented.

**Idea.** Turn the KiwiSDR map selector (`core/src/gui/brown/kiwisdr_map.*`,
`kiwisdr_directory.*`, on `geomap.*`) into one browser for every networked
source:

* **Directory providers** supply receivers;
* **each source module** supplies a prober and a connect handler;
* **one map + list UI** in core shows them all, with filters, testing and
  connecting.

## Why now

* **Three public directories exist for sources we have or plan:**

  | directory | endpoint | entries (2026-11-24) | position | capacity | other fields |
  |-----------|----------|----------------------|----------|----------|--------------|
  | KiwiSDR | `rx.linkfanel.net/kiwisdr_com.js` (mirror of kiwisdr.com/public) | 842 | GPS | `users` / `users_max` | `bands`, `snr`, `sdr_hw`, `ext_api`, `mode` |
  | UberSDR | `instances.ubersdr.org/api/instances` (JSON) | 77 | lat/lon, Maidenhead | `available_clients` / `max_clients` | `tls`, `public_iq_modes`, `tuning_range`, SNR, features, `load_status` |
  | SpyServer | `airspy.com/directory/status.json` (JSON) | 218 | `antennaLocation` lat/long | `currentClientCount` / `maxClients` | `deviceType`, min/max frequency, `maximumIQSampleRate`, `fullControlAllowed`, `streamingHost` / `streamingPort`, `online`, `lastSeen` |

* **The map exists but is KiwiSDR-only.** It is
  `KiwiSDRMapSelector` + `KiwiSDRDirectoryClient` + `KiwiSDRTester`, with
  `ServerEntry` holding Kiwi fields. `GeoMap` (pan, zoom, hit-test,
  tooltips) is already generic.
* **Two users already share it:** `kiwisdr_source` and `websdr_view`.
* **The SpyServer directory works today:** plain TCP, no TLS, and
  `spyserver_source` exists. KiwiSDR TLS hosts and UberSDR wait on the TLS
  work ([kiwisdr-streams.md](../research/kiwisdr-streams.md) §7,
  [ubersdr.md](../research/ubersdr.md)).

## Design

### Model (core, `core/src/gui/remote/`)

```cpp
enum class RemoteKind { KiwiSDR, UberSDR, SpyServer, SDRIAKServer, RtlTcp, NetSDR, Hermes, Other };

struct RemoteTarget {          // what a source needs to connect
    std::string scheme;        // "http", "https", "tcp"
    std::string host;
    int         port = 0;
    std::string path;          // optional
};

struct RemoteEntry {
    std::string id;            // stable: provider + host:port (or directory id)
    RemoteKind  kind;
    std::string provider;      // "kiwisdr.com", "ubersdr.org", "airspy.com", "recent", "lan"
    std::string name, location;
    std::optional<ImVec2> latLon;          // absent: list view only
    RemoteTarget target;
    std::optional<int> users, usersMax;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;   // served frequency ranges, Hz
    std::optional<float> snrDb;
    bool tlsRequired = false;
    std::vector<std::pair<std::string, std::string>> details;  // shown in the panel, already formatted
    std::vector<std::string> flags;        // "full", "no-control", "wide-iq", "ext-api", "waterfall"...
};
```

* **Personal data.** The SpyServer and KiwiSDR directories publish owner
  e-mail addresses. Providers **drop e-mail fields at parse time**; they
  are never shown or cached.
* **One host, two protocols.** When one host appears in two directories
  (3 UberSDRs are also in the KiwiSDR list), the browser shows one marker
  with both connect options, the native one first.

### Providers

* **The interface:**

  ```cpp
  struct DirectoryProvider {
      virtual std::string name() const = 0;
      virtual RemoteKind kind() const = 0;
      // Worker thread. Gentle: one request, a timeout, honours the proxy setting.
      virtual std::vector<RemoteEntry> fetch() = 0;
  };
  ```

* **The browser around it:**
  * It runs fetches on one background thread, one provider at a time.
  * It caches each result on disk with its fetch time, so the map opens
    instantly and offline.
  * It refetches only when the cache is older than about 15 min or the
    user asks.
  * It uses `If-Modified-Since`/ETag where the server supports them.
* **Built-in providers:** KiwiSDR (the current parser, moved), UberSDR and
  SpyServer, all plain HTTP + JSON.
* **Receiverbook** (for `openwebrx_source`, plan
  [network-sources/openwebrx/](network-sources/openwebrx/README.md)):
  * it has no API, but `https://www.receiverbook.de/map` embeds every
    receiver as a JSON literal, `var receivers = [...]` (one request of
    about 380 kB);
  * each site has a GeoJSON position and receivers with
    `{label, url, type, version}`, `type` being `OpenWebRX`, `KiwiSDR` or
    `WebSDR` (openwebrx.md §1);
  * the provider offers OpenWebRX entries of version 1.2.129 or later,
    and shows older ones greyed as audio-only;
  * its KiwiSDR entries can be merged into the KiwiSDR provider by URL.
* **Recent and favourites.** A provider backed by core config, holding
  every target the user connected to or saved, for any kind. It covers
  `sdriak_server_source`, `rtl_tcp_source`, `rfspace_source` and
  `hermes_source`, which have no public directory.
* **LAN discovery (optional, later).** HPSDR discovery broadcast, NetSDR
  UDP discovery, mDNS for SDRIAK servers. List-only; there are no
  coordinates.

### Registration (no core → module dependency)

A source module registers what only it can do:

```cpp
struct RemoteHandler {
    RemoteKind kind;
    std::string sourceName;                                  // as registered with SourceManager
    std::function<ProbeResult(const RemoteEntry&, std::atomic<bool>& cancel)> probe;   // worker thread
    std::function<void(const RemoteEntry&)> connect;         // UI thread
};
sigpath::remoteRegistry.registerHandler(handler);            // in the module constructor
```

* **What a probe does,** per protocol:
  * **KiwiSDR:** `/VER`, then SND until the first packet (the current
    tester).
  * **UberSDR:** `POST /connection`, then the first `iq48` packet.
  * **SpyServer:** handshake until `DEVICE_INFO` and `CLIENT_SYNC`; report
    `canControl`.
  * **Others:** connect plus first data.
* **Probe policy.** Probes run one at a time and can be cancelled.
* **Probe result:** OK or failed, the reason (`too_busy`, `badp`, TLS
  required, refused), measured round-trip time, and what was negotiated
  (rates, modes).
* **What Connect does:**
  `sigpath::sourceManager.selectSource(handler.sourceName)`, then
  `handler.connect(entry)`, which fills the module's host/port/options and
  connects as if typed.
* **Kinds with no handler** (module not loaded or not built) stay listed
  but greyed, with the reason.

### UI (`RemoteBrowser`, replacing `KiwiSDRMapSelector`)

* **Map view.** `GeoMap` with one marker shape per kind (Kiwi ●,
  UberSDR ◆, SpyServer ▲), fill by state: free, nearly full, full, needs
  TLS (not yet supported), not connectable. Keep the current behaviour:
  cluster picker, phone-landscape footer, hide-full toggle.
* **List view.** A sortable table: name, kind, location, distance from the
  user's QTH (the band-stack/spots QTH setting), users, ranges, SNR, last
  seen; with search. On phones, the list is often the better view.
* **Filters:**
  * kind;
  * "covers the current frequency" (uses `ranges`);
  * free slots;
  * TLS-capable only while TLS is missing;
  * full control (SpyServer);
  * waterfall available.
* **Panel.** The selected entry's `details`, plus **Test**, **Connect** and
  **Favourite**.
* **Entry points:**
  * **Source menu:** a "Browse remote receivers…" button opens the browser
    with every kind.
  * **Per source:** each network source's own "Browse…" opens it filtered
    to its kind; that replaces the current Kiwi map button.
  * **`websdr_view`:** opens it filtered to KiwiSDR.

### Gentleness

* **Directory fetches:** one request per provider per refresh, sequential,
  cached.
* **Probes:** user-initiated, one at a time, short, closed cleanly.
* **No background sweeps.** Nothing probes many receivers unattended. A
  "test all visible" action, if ever added, is sequential with pauses and
  capped.
* **One network layer.** Everything honours the same proxy and TLS
  settings (see [forks/zh.md](../research/forks/zh.md) §2 for the proxy
  layer), so the browser and the sources behave the same behind a proxy.

## Steps

1. **Extract, behaviour-neutral.** Move `GeoMap`, the marker, cluster and
   panel code and the Kiwi parser into `core/src/gui/remote/`, behind
   `RemoteEntry` and a `KiwiSDRProvider`. `kiwisdr_source` and
   `websdr_view` keep working, filtered to KiwiSDR.
2. **Registry and handoff.** Add `RemoteHandler` and the Connect flow.
   Move `KiwiSDRTester` into `kiwisdr_source` as its probe.
3. **SpyServer provider and handler.** Works today and gives 218
   receivers. The probe shows `canControl`, because many servers are
   shared.
4. **List view, filters and favourites/recent** for every kind, including
   the sources without a directory.
5. **UberSDR provider,** together with `ubersdr_source`, after TLS.
6. **De-duplication across directories** and native-first connect options.
7. **Optional:** LAN discovery providers.

## Open questions

* **The "covers the current frequency" filter.** It needs a defined
  current frequency when no source is running: the last tuned one, from
  the band stack.
* **SpyServer `fullControlAllowed`.** It is true on 217 of 218 servers,
  yet `canControl` depends on who else is connected. Show it from the
  probe, not from the directory.
* **Directory refresh cadence and cache location:** `core::args["root"]`
  plus `remote_cache/`.
