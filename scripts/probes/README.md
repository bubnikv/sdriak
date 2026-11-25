# Protocol probes for networked receivers

These are standalone Python scripts (stdlib plus numpy) that measure how
remote receivers behave on the wire. They connect to **third-party public
servers**, so keep to the rules below. Results from these scripts are written
up in `doc/research/kiwisdr-streams.md` §8, `doc/research/ubersdr.md` §4 and
`doc/research/spyserver-streams.md`.
The plans that build on them are in `doc/todo/network-sources/`.

They are not part of the build and are not imported by anything.

## Rules for running them

- **One session at a time.** Pause between sessions and keep each one short.
  Never sweep many servers in parallel: a 48-way `/VER` sweep once got
  `proxy.kiwisdr.com` to stop answering.
- **Leave capacity for others.** Check `/status` (KiwiSDR) or the directory
  (UberSDR, SpyServer) first, and pick receivers where at least two slots
  stay free.
- **Identify honestly.** The scripts identify themselves as `sdriak-test`.
- **Run from the probe directory itself** (`cd scripts/probes/<kind>`). The
  scripts import each other by file name. Write outputs to a scratch
  directory, not into the repository.

## KiwiSDR (`kiwisdr/`)

| script | purpose |
|--------|---------|
| `kiwiprobe.py` | library and main session: SND + W/F pairing, setup messages, `NEW_FREQ`, zoom geometry, frame rates, `seq` lag, keepalive, rates. `python kiwiprobe.py <host> <port> <outdir> <test> [http\|https]`, where the test is `plainws`, `k1`, `k3`, `main`, `main2` or `bins` |
| `kiwi_tests.py` | second-batch tests on top of `kiwiprobe.py`: refusal, kiwiclient path on a full server, retune switch offset, fast retunes, back-pressure, limits, converter offset, 32 MHz span, GPS timing, panning, Kiwi-compatible servers, TLS certificates, ADPCM. `python kiwi_tests.py <test> <scheme> <host> <port>` |
| `run_batch.py`, `run_batch3.py` | gentle sequential runners with a `/status` gate and recorded random seeds |
| `probe_list.py` | **HTTP only, no user slot**: classify a downloaded receiver list by reachability (plain, redirect, TLS) |
| `connect_one.py` | one short SND session exactly as our client connected in 2026-11 (`/kiwi/<ts>/SND`) |

`run_batch3.py` reads the receiver list and the reachability results from
`KIWI_LIST` and `KIWI_REACH`:

```
curl -o linkfanel.js http://rx.linkfanel.net/kiwisdr_com.js
python probe_list.py linkfanel.js probe2.json
```

## UberSDR (`ubersdr/`)

| script | purpose |
|--------|---------|
| `ubersession.py` | one native-API session: `/api/description`, `POST /connection`, `/ws` `iq48`, `/ws/user-spectrum` v2. It retunes, changes the view and records raw frames to a pickle |
| `ubersession2.py` | follow-up sessions: `vfo`/`vfo2` (secondary VFOs, back-pressure), `ret` (retunes, drag, far jumps, reconnect), `lvl`/`lvl2` (levels, spectrum edges, `set_rate`), `final` (360-sample packets, secondary retunes, `iq96`/`iq192`) |
| `uberanalyze.py`, `uberanalyze2.py` | offline analysis of those recordings: IQ timestamps, the retune switch relative to the `status` echo, spectrum v2 decoding (FFT-order bins), alignment, levels |

The analysers need UberSDR's own `pcm_v4.py` decoder (GPL-3.0,
madpsy/ka9q_ubersdr). It is **not vendored here**. Download it into a
directory of its own and point `UBERSDR_LIB` at it:

```
mkdir ubersdr_lib
curl -o ubersdr_lib/pcm_v4.py https://raw.githubusercontent.com/madpsy/ka9q_ubersdr/HEAD/clients/python/pcm_v4.py
UBERSDR_LIB=$PWD/ubersdr_lib UBERSDR_PCMV4_PURE=1 python uberanalyze.py session.pkl
```

Pick instances from `https://instances.ubersdr.org/api/instances`: online,
TLS, plenty of `available_clients`, `load_status` `ok`.

## SpyServer (`spyserver/`)

| script | purpose |
|--------|---------|
| `spyprobe.py` | one session over the raw TCP protocol, FFT+IQ mode: `python spyprobe.py <host> <port> <outdir> <test>`. `main`: IQ retunes with `PING`, `CLIENT_SYNC`/`PONG` positions in the IQ stream, FFT stages, scaling, pixels. `main2`: FFT geometry per stage against the IQ spectrum. `scale`: dB scaling and IQ placement with settled 6 s windows (settings apply one to three seconds late). `shared`: a second client without control. `gainorder`: digital gain before and after `STREAMING_MODE` |
| `spyanalyze.py` | offline analysis of `main`, `main2` and `scale` recordings: retune switch against the `PONG`, span fit, dB-mapping fit, IQ placement against the FFT |

Pick servers from `https://airspy.com/directory/status.json`: online,
`fullControlAllowed`, at least three free slots. Many listed servers are not
reachable from outside; check with a plain TCP connect first, one at a time.
