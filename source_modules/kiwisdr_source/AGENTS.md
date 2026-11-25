# KiwiSDR Source Notes

## Receiver list and map

KiwiSDR receivers are picked in the core remote-receiver browser
(`core/src/gui/remote/`, plan `doc/todo/remote-receiver-browser.md`), which
lists every networked source's receivers. The menu's **Browse...** opens it
filtered to KiwiSDR; `websdr_view` holds a view of its own with "Add".

The base map is drawn locally by `geomap` from the resources file
`cty/map.json`; no tiles are downloaded. The web is used only for the
directory documents.

The pieces, and what to update together:

- `core/src/gui/remote/providers.cpp` — `parseKiwiSDRDirectory` and the
  KiwiSDR provider (one request per refresh).
- `core/src/gui/remote/directory.{h,cpp}` — the shared fetch thread and
  the cache, `<root>/remote_cache/kiwisdr.com.json` (parsed entries, fetch
  time, ETag/Last-Modified). Refetched when older than 15 min or on
  Refresh; a failure keeps the cached list.
- `core/src/gui/remote/browser.{h,cpp}` — map and list views, filters,
  Test/Connect/Favourite.
- `src/kiwisdr_probe.cpp` — this module's Test: `probeKiwiSDR`.
- `src/main.cpp` — registers the `RemoteHandler` (`supportsTls = false`:
  the SND client speaks plain http) and `connectRemote`, which goes
  through `commitServer` and starts playing.
- `core/tests/remote_directory_tests.cpp` — parser tests; `--files` parses
  saved real documents.

## Endpoint

```text
GET http://rx.linkfanel.net/kiwisdr_com.js
```

A mirror of kiwisdr.com/public; it serves no TLS. The response is
JavaScript:

```js
var kiwisdr_com = [
  { ... },
  { ... },
]
;
```

`cleanKiwiJs` extracts the array by literal-token search: the prefix
`var kiwisdr_com =`, the **last** `},\n]\n;` (LF only), and appends `}]`.
This is brittle by design; a changed trailing format fails the fetch and
the cached list stays. The body is capped at 4 MiB. On 2026-11-25 it
parsed to 838 online receivers.

## Per-entry fields

`parseKiwiEntry` drops an entry without `gps`, `name`, `url`, `loc`, `snr`,
`users`, `users_max` or `offline`, or with `offline != "no"`, or with a
URL that is not http(s). E-mail fields are never read.

| Field | Use |
|-------|-----|
| `gps` | `"(lat, lon)"`, parsed with `std::locale::classic()` |
| `name`, `loc` | name and location |
| `url` | target; `https` sets `tlsRequired`, which greys the entry |
| `bands` | `"lo-hi"` Hz, the served range; a malformed value leaves no range |
| `snr` | first value as `snrDb` |
| `users`, `users_max` | occupancy; `users >= users_max` with `users_max > 0` is full |
| `ext_api` | flag `ext-api` when > 0 (the "KiwiSDR: EXT API only" filter) |
| `antenna`, `sdr_hw`, `sw_version` | panel rows |

Entries are sorted by SNR ascending, so the best stations paint on top.
Receiverbook also lists KiwiSDRs; they merge with these by host:port, this
provider's entry winning.

## Test (probe)

`probeKiwiSDR` connects `KiwiSDRClient` to `host:port`, tunes IQ to the
middle of the served range (14.074 MHz without one), waits up to 5 s for
the first SND samples, and closes. It reports the refusal reason
(`too_busy`, `badp`, ...), the time to first samples, the IQ rate and the
served range. The core registry runs one probe at a time and cancels it
when the module unregisters.
