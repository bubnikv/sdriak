# SpyServer 0: protocol measurements (no product code)

Date: 2026-11-24. Status: done. The answers are in
[spyserver-streams.md](../../../research/spyserver-streams.md); the probe is
`scripts/probes/spyserver/`. Commit rule chosen: **B (`PONG`), with C as
the fallback**. Part of [README.md](README.md).

jp's code leaves facts open that the waterfall geometry and the commit rule
depend on. Measure them with a standalone Python probe, a new `scripts/probes/spyserver/`
modelled on `scripts/probes/kiwisdr/` and `scripts/probes/ubersdr/`, and on the findings in
([../../../research/kiwisdr-streams.md](../../../research/kiwisdr-streams.md) §8,
[../../../research/ubersdr.md](../../../research/ubersdr.md) §4).

## Targets

* **Selection.** Draw from `airspy.com/directory/status.json`:
  `online`, `maxClients ≥ 3` with at least 2 free (many servers allow 1–2
  clients), and `fullControlAllowed`.
* **Coverage:** at least one Airspy HF+ (the TDF/DCF77 carriers work in
  Europe), one Airspy One/R2 (an FM broadcast carrier), and if possible one
  with a capped `maximum_bandwidth`.
* **Gentleness:** one session at a time, about 60 s, with pauses; never a
  parallel sweep.

## Questions

1. **`CLIENT_SYNC` after settings.** Is `SPYSERVER_MSG_TYPE_CLIENT_SYNC`
   sent after `IQ_FREQUENCY`, `FFT_FREQUENCY` and `FFT_DECIMATION`, and
   what do `IQCenterFrequency` and `FFTCenterFrequency` report? Log every
   message with its arrival and the count of IQ samples since the command.
2. **Where the switch lands.** With a steady carrier at +5 kHz, retune by
   2 kHz six times. Locate the switch sample (filtered instantaneous
   frequency, as in the UberSDR analysis) relative to the sync message.
   Is it before or after, and at a constant offset?
3. **PING/PONG as a marker.** Send `PING` right after a setting. Does the
   `PONG` arrive after the switched samples (an in-band marker even
   without `CLIENT_SYNC`)?
4. **FFT scaling.** Vary `FFT_DB_OFFSET` and `FFT_DB_RANGE` and record a
   fixed carrier and the noise floor. Confirm or correct jp's
   `dB = offset − range·(1 − b/255)`.
5. **FFT span.** Place a carrier at a known offset and check which bin it
   lands in at stages 0, 2 and 4. Do the bins cover
   `MaximumBandwidth / 2^n` (jp's assumption) or `MaximumSampleRate / 2^n`?
   Do they follow the bin-edge or bin-centre convention?
6. **`FFT_DISPLAY_PIXELS` and frame rate.** Is the requested pixel count
   honoured? How do frames/s compare with the directory's `displayFPS`?
7. **Slide limits.** On a capped server, how far can `IQ_FREQUENCY` move
   from the device centre? Do `CLIENT_SYNC`'s `Min/MaximumIQCenterFrequency`
   match?
8. **Shared mode.** With a second client holding control
   (`canControl = 0`): what can still be set, and within which limits?
9. **Gain order.** jp found that digital gain sent before `STREAMING_MODE`
   over-drives the IQ. Compare the IQ RMS for both orders.

## Output

* Record the answers as
  `doc/research/spyserver-streams.md` (new; same structure as
  kiwisdr-streams.md §8).
* Choose the commit rule for SpyServer 4, in order of preference:
  * **A:** `CLIENT_SYNC` after the switch;
  * **B:** `PONG` after the switch;
  * **C:** a timeout.
