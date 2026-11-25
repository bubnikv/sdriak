# OpenWebRX+: IQ source with the server's waterfall

Date: 2026-11-25. Status: steps 1–3 implemented and committed. The client
passed its live check on two receivers; the module has not been run in
the GUI. Part of [../README.md](../README.md).

A new `source_modules/openwebrx_source` for OpenWebRX+ receivers of
version 1.2.129 or later, the versions with the `iq` mode. It gives:

* **48 kS/s IQ** around an offset the client chooses inside the server's
  band (its "profile");
* **the server's waterfall** over the whole profile span (1–4 MHz on the
  receivers measured), one fixed span with no server zoom.

On 2026-11-25, Receiverbook listed 420 OpenWebRX receivers, 195 of them
already on 1.2.129 or later. Upstream OpenWebRX and older OpenWebRX+ send
demodulated audio only. This source refuses them with a reason instead of
playing audio.

Everything here comes from the OpenWebRX+ source
([luarvique/openwebrx](https://github.com/luarvique/openwebrx) `0b91fb5d`,
1.2.131) and three live sessions; see
[../../../research/openwebrx.md](../../../research/openwebrx.md). OpenWebRX
is AGPL-3.0: these plans take protocol facts from its source and copy no
code. The ADPCM decoders are written from the IMA ADPCM algorithm and the
framing described in the research doc.

## How it differs from the sources already built

| | KiwiSDR | UberSDR | OpenWebRX+ |
|-|---------|---------|------------|
| IQ | 12 kHz | 48–384 kHz | 48 kHz, AGC'd, clipped at 0.8, int16 or 4-bit ADPCM |
| centre | free | free | fixed by the profile; a profile change moves the band **for every listener** on that SDR |
| server zoom | 0…zoom_max | continuous | none: one span per profile |
| retune marker | `NEW_FREQ` flag | status echo | **none**: commit by arrival, measured 167–246 ms after the command |
| frame timing | `seq` | timestamps | none: by arrival |
| slow client | buffered | buffered | **disconnected** when the server's frame queue fills |

## Sub-plans

| plan | prerequisites | content |
|------|---------------|---------|
| [1-client.md](1-client.md) | transport/1–2 | `/ws/` handshake, `connectionproperties`, `dspcontrol`, config and profile messages, binary frame types, both ADPCM decoders, reading without stalls |
| [2-source-module.md](2-source-module.md) | 1 | module, UI, version and `iq`-mode check, fixed centre from the profile, profile change behind a warning and a rate limit, prebuffer, config, Android |
| [3-waterfall-and-window.md](3-waterfall-and-window.md) | 2, core 1–5 | the profile FFT as a `SpectrumProvider`, the IQ window as `offset_freq` with an arrival-timed commit |

Directory: Receiverbook's `/map` JSON belongs to
[../../remote-receiver-browser.md](../../remote-receiver-browser.md), not
here.

## Review gate for the whole source (user tests)

* An OpenWebRX+ 1.2.131 receiver plays SSB and AM through our
  demodulators. The waterfall shows its whole profile.
* Dragging a VFO inside the IQ window gives no retune. Dragging it out
  moves the window with at most a short mute.
* An upstream OpenWebRX receiver is refused with a reason that names the
  version needed.
* Another listener changing the profile moves our display to the new band
  without a crash, and every VFO stays inside the span.
