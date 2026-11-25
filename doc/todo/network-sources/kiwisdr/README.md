# KiwiSDR: client fixes and server waterfall

Date: 2026-11-24. Status: current. Part of [../README.md](../README.md).

Everything here comes from
[../../../research/kiwisdr-streams.md](../../../research/kiwisdr-streams.md):
the protocol read from the server source, and measurements on 20+ public
receivers in 2026-11. Section numbers below refer to it.

## Code involved

* **Protocol client:** `core/src/utils/proto/kiwisdr.{h,cpp}`
  (`KiwiSDRClient`). It is used by `source_modules/kiwisdr_source` (IQ
  mode) and `misc_modules/websdr_view` (`TUNE_REAL` mode, several clients
  at once). **Both must keep working.**
* **Map, directory and tester:** `core/src/gui/brown/kiwisdr_{map,directory}.*`
  (generalised separately: `../../remote-receiver-browser.md`).

## Sub-plans

| plan | prerequisites | content |
|------|---------------|---------|
| [1-client-fixes.md](1-client-fixes.md) | transport/1–2 | TLS via URL scheme, the krec path, flag and size handling, server-reported rates, refusal surfacing, flavour detection |
| [2-waterfall.md](2-waterfall.md) | 1, core 1/3/4/5 | W/F client, `KiwiSpectrumProvider`, zoom ladder, presentation by `seq` |
| [3-iq-window.md](3-iq-window.md) | 2, core 2 | IQ window inside the server span, `NEW_FREQ` commit through the link marker |
