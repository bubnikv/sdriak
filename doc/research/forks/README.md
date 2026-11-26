# SDR++ forks, modules and utilities

Working survey of the fork landscape: what each fork carries and whether it is
worth merging. Kept as raw notes on purpose — one line of description, then the
links. Forks that got a full review have their own file here:

- [Brown (sannysanoff)](brown.md) — TX, FT8, noise reduction, mobile UI; re-reviewed 2026-08-09 (agent/e2e test infra, 3 small fixes worth taking)
- [M0OPK Brown panadapter fork](m0opk.md) — fixed-IF/mode-offset infrastructure, two-way Icom/Yaesu/rigctl control, filter sync; QMX architecture comparison
- [qrp73](qrp73.md)
- [ericek111](ericek111.md)
- [qmx-panadapter (SteffenLav)](qmx-panadapter.md)
- [Community Edition (LunaeMons)](community-edition.md)
- [LDighera / WB6BBB](ldighera.md) — AI-assisted Pi5 fork: S-meter, manual notch, SSB AGC, CW/SSTV/FreeDV/HD Radio
- [SDRIAK zh (lzh173)](zh.md) — tracks us; Windows non-ASCII paths, Android file source + document picker, KiwiSDR HTTP proxy, seekable file playback, FT8/FT4 on ft8_lib, Simplified-Chinese UI
- [SDR++ jp (jprincl)](jp.md) — Linux window-ID/AppImage fixes, EiBi/Aoki Frequency Info, FT8/FT4/WSPR, F4JTV-sourced WEFAX/SSTV/RTTY, AGC ceiling, SpyServer VFO+FFT, Android GL-context loss
- [aurimasniekis](aurimasniekis.md) — ESP32 antenna switcher module (skip); worth taking: atv_decoder teardown order, two z-order gaps, and a pointer to a real RtAudio macOS encoding bug

## Survey

TX, a lot more
https://github.com/sannysanoff/SDRPlusPlusBrown
https://sdrpp-brown.san.systems/tx.html
https://github.com/sannysanoff/SDRPlusPlusBrown/blob/master/changelog.md

HAM worthy modifications?
https://github.com/qrp73/SDRPP

SDR-888, 1 star
https://github.com/howard0su/SDRPlusPlus

SideKiq SDR hardware integration, 4 stars
https://github.com/alphafox02/SDRPlusPlus/commits/sidekiq/

RigExpert fork: fobossdr, network sink catch block, spectran_source fix, 3 stars
https://github.com/rigexpert/SDRPlusPlus

OM2LT Erik Brocko: MiriSDR - open source SDRPlay driver, PortAudio diff, SoapySDR diff, 3 stars
https://github.com/ericek111/SDRPlusPlus

NRSC-5 decoder
https://github.com/mriise/SDRPlusPlus-NRSC-5

Changed PlutoSDR source to support libiio v1 API - the libiio v0 crashes for me
https://github.com/tilarids/SDRPlusPlus

Muting M17 when encrypted
https://github.com/AlexandreRouma/SDRPlusPlus/compare/master...Paulo-D2000:SDRPlusPlus:master

support for https://rfnm.com/
https://github.com/chiaraberti13/SDRPlusPlus

Chinese version BG5JRE, localization?
https://github.com/nkxingxh/SDRPlusPlus-CHS

IMGUI update to support OpenGL 3.0 correctly https://github.com/AlexandreRouma/SDRPlusPlus/issues/1430
https://github.com/sdekrijger/SDRPlusPlus

Side controls for Vol, Zoom, FFTMin, FFTMax
https://github.com/r4d10n/SDRPlusPlus

Caribou Lite support https://www.crowdsupply.com/cariboulabs/cariboulite-rpi-hat
https://github.com/cariboulabs/SDRPlusPlus

Baseband sink
https://github.com/SatDump/SDRPlusPlus

PlutoSDR tweaks, 17 stars
https://github.com/F5OEO/SDRPlusPlus

Fobos Agile RX support (RigExpert)
https://github.com/stenn930/SDRPlusPlus

SDR++++
https://github.com/Bas-W/SDR4P/branches

Docking layout, updated ImGUI, 1 star
https://github.com/Zaryob/SDRPlusPlus
https://github.com/AlexandreRouma/SDRPlusPlus/compare/master...Zaryob:SDRPlusPlus:master

?? ATV, raw audio saving?
https://github.com/randomradioprojects/SDRPlusPlus

?? PlutoSDR fixes
https://github.com/meee1/SDRPlusPlus

?? M17 encryption mute?
https://github.com/Paulo-D2000/SDRPlusPlus

?? Some UI tweaks, airspy_TCP server
https://github.com/daviderud/SDRPlusPlus

?? Some MacOS compilation fixes, audio in support for server
https://github.com/noah04n/SDRPlusPlus/

?? active, chinese? USRP?
https://github.com/JustZhenya/SDRPlusPlus

WB6BBB, AI-assisted, Pi5/Trixie Debian packages, 4 stars — reviewed, see ldighera.md
https://github.com/LDighera/SDRPlusPlus
https://github.com/LDighera/SDRPlusPlus/tree/release/1.3-beta-20260723
beta test thread, 83 msgs (bot-walled, needs a browser):
https://groups.io/g/linuxham/topic/sdr_1_3_source_beta_preview/118709023

SDDC, ???
https://github.com/syehorov/SDRPlusPlus

ESP32 antenna switcher module (ESPHome API), audio device-name UTF-8 sanitization,
floating-window z-order fixes, 0 stars — reviewed, see aurimasniekis.md
https://github.com/aurimasniekis/SDRPlusPlus

LimeSDR fixes, 2 stars
https://github.com/alphafox02/SDRPlusPlus

R-PI hardware (rotary encoder), 1 star
https://github.com/K7MDL2/SDRPlusPlus
https://github.com/AlexandreRouma/SDRPlusPlus/compare/master...K7MDL2:SDRPlusPlus:master

Disconnected SDR++ forks
https://github.com/LunaeMons/SDRPlusPlus_CommunityEdition
https://github.com/KubaPro010/SDRMinusMinus

SDR++ modules
https://github.com/cropinghigh/sdrpp-tetra-demodulator
https://github.com/dbdexter-dev/sdrpp_radiosonde
https://github.com/williamyang98/SDRPlusPlus-DAB-Radio-Plugin
https://github.com/cropinghigh/sdrpp-vhfvoiceradio
https://github.com/cropinghigh/sdrpp-inmarsatc-demodulator
https://github.com/Sultan-papagani/sdrpp_new_rtlsdr_source
https://github.com/cropinghigh/sdrpp-dvbs-demodulator
https://github.com/d3cker/sdrppcontroller
https://github.com/OttoPattemore/shortwave-station-list-sdrpp
https://github.com/gerner/sdrpp-rigsync
https://github.com/F4JTV/sdrpp_cospas_sarsat
https://github.com/BlackDuke07/sdrpp-spyglass
https://github.com/comparchitect/sdrpp-pico-panel
https://github.com/pwnderpants/sdrpp-noise-reduction uses https://github.com/pwnderpants/sdrpp-noise-reduction/tree/main/src/sdrpp_noise_reduction over UDP
https://github.com/srgkmv/sdrpp-micron-source
https://github.com/srgkmv/sdrpp-libresdr-source

Analog meter with signal / noise
https://github.com/MatiasSaibene/S-Meter_for_SDRPP

BlackDuke07/sdrpp-bm-scanner: Bookmark Scanner plugin for SDR++
https://github.com/BlackDuke07/sdrpp-bm-scanner

SDR++ utils

Quick demo synchronising SDR++ and a rig (via hamlib)
https://github.com/mgiugliano/syncSDRpp

https://project.crx.cloud/ping-sound-hamradio-remote-station

bladeRF Support on Android #1752
https://github.com/AlexandreRouma/SDRPlusPlus/issues/1752
https://github.com/rickmark/SDRPlusPlus/commit/348e2b157fa5b8be21103d23360164c88b47031e


integrated:
Windows ARM64 https://github.com/fluid41/SDRPlusPlus/commit/19e31e03c06df8aa95ad0ec23c06ff3d79e015e7
Radiosonde https://github.com/jethrocarr/SDRPlusPlus
Spots https://github.com/gerner/sdrpp-spots
