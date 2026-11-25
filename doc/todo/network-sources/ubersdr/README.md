# UberSDR: native remote source with server waterfall

Date: 2026-11-24. Status: current. Part of [../README.md](../README.md).

A new `source_modules/ubersdr_source` that speaks UberSDR's native API.
It gives:

* **wide IQ:** 48–384 kS/s, depending on what each session is allowed;
* **a self-describing server waterfall** with continuous zoom;
* **later, secondary 12 kHz VFO streams.**

Everything here was read from the UberSDR source (GPL-3.0,
madpsy/ka9q_ubersdr, v0.1.69) and measured on 8 instances; see
[../../../research/ubersdr.md](../../../research/ubersdr.md) §3–§4.2. The
KiwiSDR emulation some instances offer is not used: it is opt-in and
narrower.

## Sub-plans

| plan | prerequisites | content |
|------|---------------|---------|
| [1-protocol-client.md](1-protocol-client.md) | transport/1–2 | `/connection`, `/ws` IQ with the `pcm_v4` decoder, status/tune/error/ping, session id |
| [2-source-module.md](2-source-module.md) | 1 | module, UI, IQ mode choice and link-rate guard, prebuffer, config, Android |
| [3-waterfall.md](3-waterfall.md) | 2, core 1/3/4/5 | `/ws/user-spectrum` v2 client, FFT-order rotation, continuous zoom ladder, presentation by timestamp |
| [4-vfos.md](4-vfos.md) | 3, core 2 | IQ window in the wide stream, echo-based commit, secondary `vfo=N` streams for VFOs outside it |
