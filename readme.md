# SDRIAK — Ride the Radio Waves

<img src="root/res/icons/sdriak.png" alt="SDRIAK logo" width="128">

SDRIAK is a cross-platform, open source SDR application for Windows, Linux, macOS and Android. It is a fork of [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) by Alexandre Rouma, maintained by Vojtech Bubnik (OK1IAK), and it combines work from several other SDR++ forks and modules, see [Credits](#credits).

**Please do not report bugs in this fork to the original author. Report them here, in [Issues](https://github.com/bubnikv/sdriak/issues).**

* [Releases](https://github.com/bubnikv/sdriak/releases)
* [Google Play](https://play.google.com/store/apps/details?id=org.ok1iak.sdrpp)
* [Changelog](changelog.md) ([full history](changelog-full.md))

## Features

SDRIAK keeps the features of SDR++, such as multiple VFOs, SIMD accelerated DSP, a modular plugin design and support for a wide range of SDR hardware. It adds:

### QMX transceiver

* Direct USB IQ source on Windows, Linux, macOS and Android, with bidirectional CAT synchronization of frequency and mode
* Panadapter operation: with Sync VFO enabled, the VFO stays at the center of the waterfall at every zoom level
* Remote reception of a QMX attached to another machine, for example through the [Android server app](https://github.com/bubnikv/qmxserver-android)

### Touch and Android

* Touch-friendly interface with Android-sized controls, also available as a preview on desktop
* Bands / Frequency dialog with a direct-entry keypad and three-register band stacking
* Multi-touch waterfall zoom and reworked USB device access for all USB sources

### Sources and modules

* KiwiSDR client with a world map of servers, and a WebSDR view
* Radiosonde decoder, spots on the waterfall and a frequency manager with scheduled bookmarks
* Native macOS CoreAudio output
* Password authentication for the SDRIAK Server. **The network protocol is not compatible with upstream SDR++.**

### Radio and recording

* Manual/auto AGC, a signal level meter with peak hold and squelch hysteresis
* Seven FFT windows, with FFT levels calibrated to dBFS
* LogMMSE/OMLSA noise reduction
* FLAC, Opus and 24-bit PCM recording, and playback of FLAC and extended WAV files (RF64, float, multi-channel)

## Installing

Download the latest version from the [Releases page](https://github.com/bubnikv/sdriak/releases).

* **Windows:** extract the x64 or ARM64 `.zip` and run the program from the extracted folder.
* **Linux:** install the `.deb` matching your Debian or Ubuntu release, or run the x86_64 or aarch64 `.AppImage`, see [doc/appimage.md](doc/appimage.md).
* **macOS:** extract the Apple Silicon (`arm`) or Intel `.zip`.
* **Android:** install from [Google Play](https://play.google.com/store/apps/details?id=org.ok1iak.sdrpp) or use an `.apk` from the Releases page.

## Building

All third-party libraries are built from source by the CMake project in [deps](deps/CMakeLists.txt). See [doc/build-windows.md](doc/build-windows.md) for Windows, and the [CI workflows](.github/workflows) for the exact steps on every platform.

## Contributing

Please read [contributing.md](contributing.md), and open an issue to discuss a bug fix or feature first.

## Credits

SDRIAK is built on the work of many people. Thanks and due respect to all of them.

* **Alexandre Rouma (ON5RYZ)**, the author of [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus), and all its contributors, patrons and hardware donators. They are listed on the upstream page and in the About dialog of the application.

### Merged projects

| Project | Author | What SDRIAK uses from it |
| --- | --- | --- |
| [SDR++Brown](https://github.com/sannysanoff/SDRPlusPlusBrown) | Sanny Sanoff | KiwiSDR source and map, WebSDR view, WebSocket client, noise reduction module, macOS CoreAudio sink, server password authentication |
| [SDRPP](https://github.com/qrp73/SDRPP) | qrp73 | Signal level meter, KiwiSDR map rendering (country polygons, day/night terminator) and client improvements, WebSocket client improvements, manual/auto AGC, squelch hysteresis and hold, extended WAV support, FFT windows and normalization, FLAC and 24-bit recording |
| [bookmark_manager](https://github.com/darauble/bookmark_manager) | Darau Ble, Davide Rovelli | Frequency manager improvements: multi-row waterfall labels, per-list colors, scheduled bookmarks |
| [shortwave-station-list-sdrpp](https://github.com/OttoPattemore/shortwave-station-list-sdrpp) | Otto Pattemore | UTC helpers of the frequency manager |
| [sdrpp_radiosonde](https://github.com/dbdexter-dev/sdrpp_radiosonde), [sondedump](https://github.com/dbdexter-dev/sondedump) | Davide Belloli | Radiosonde decoder |
| [sdrpp-spots](https://github.com/gerner/sdrpp-spots) | Nick Gerner | Spots module |
| [SDR++ fork by F5OEO](https://github.com/F5OEO/SDRPlusPlus) | Evariste Courjaud | PlutoSDR improvements |
| [SDR++ fork for HydraSDR](https://github.com/hydrasdr/SDRPlusPlus) | Benjamin Vernoux | HydraSDR RFOne source |
| [SDRPlusPlusBrownBrasil](https://github.com/leonam72/SDRPlusPlusBrownBrasil) | Leonam Ramos | Bug fixes |
| [logmmse](https://github.com/rajivpoddar/logmmse) | braindead, [wilsonchingg](https://github.com/wilsonchingg/logmmse), rajivpoddar | LogMMSE noise reduction, ported to C++ by Sanny Sanoff |
| [OMLSA-MCRA](https://github.com/xiaochunxin/OMLSA-MCRA), [OMLSA-IMCRA](https://github.com/yuzhouhe2000/OMLSA-IMCRA) | xiaochunxin, yuzhouhe2000 | OMLSA noise reduction |
| [KiwiSDR](https://github.com/jks-prv/KiwiSDR) | John Seamons (ZL4VO) | Tuning defaults in the band plans |
| [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) | Tamas Meszaros | Dependency build system |

### Contributors

* [Aang23](https://github.com/Aang23)
* [Alexsey Shestacov](https://github.com/wingrime)
* [Aosync](https://github.com/aosync)
* [attah](https://github.com/attah)
* [Benjamin Kyd](https://github.com/benkyd)
* [Benjamin Vernoux](https://github.com/bvernoux)
* [Cam K.](https://github.com/CamK06)
* [Cropinghigh](https://github.com/cropinghigh)
* [Darau Ble](https://github.com/darauble)
* [Davide Belloli](https://github.com/dbdexter-dev)
* [Davide Rovelli](https://github.com/daviderud)
* [Erik Brocko (OM2LT)](https://github.com/ericek111)
* [Evariste Courjaud (F5OEO)](https://github.com/F5OEO)
* fluid
* [Fred F4EED](http://f4eed.wordpress.com/)
* [Howard0su](https://github.com/howard0su)
* ilikehamradio
* [Jarek Surmacz](https://github.com/infrat)
* [Jiří Bubník](https://github.com/edudant)
* John Donkersley
* [Joshua Kimsey](https://github.com/JoshuaKimsey)
* [jprincl](https://github.com/jprincl)
* [konung-yaropolk](https://github.com/konung-yaropolk)
* [Leonam Ramos](https://github.com/leonam72)
* [M0OPK](https://github.com/M0OPK)
* [Manawyrm](https://github.com/Manawyrm)
* Marcelo Gadotti
* [Martin Hauke](https://github.com/mnhauke)
* [Marvin Sinister](https://github.com/marvin-sinister)
* [Maxime Biette](https://github.com/mbiette)
* [Nick Gerner](https://github.com/gerner)
* [Otto Pattemore](https://github.com/OttoPattemore)
* [Paulo Matias](https://github.com/thotypous)
* [qrp73](https://github.com/qrp73)
* [Raov](https://twitter.com/raov_birbtog)
* [Sanny Sanoff](https://github.com/sannysanoff)
* Shuyuan Liu
* [Syne Ardwin (WI9SYN)](https://esaille.me/)
* [Szymon Zakrent](https://github.com/zakrent)
* [Tamas Meszaros](https://github.com/tamasmeszaros)
* [Zaryob](https://github.com/Zaryob)
* [Zimm](https://github.com/invader-zimm)

### Libraries used

* [codec2 (David Rowe)](https://github.com/drowe67/codec2)
* [curl (Daniel Stenberg)](https://github.com/curl/curl)
* [Dear ImGui (ocornut)](https://github.com/ocornut/imgui)
* [discord-rpc (Discord)](https://github.com/discord/discord-rpc)
* [earcut.hpp (Mapbox)](https://github.com/mapbox/earcut.hpp)
* [ENet (Lee Salzman)](https://github.com/zpl-c/enet)
* [fftw3 (fftw.org)](https://www.fftw.org)
* [FLAC (Xiph.Org)](https://github.com/xiph/flac)
* [ghc::filesystem (Steffen Schümann)](https://github.com/gulrak/filesystem)
* [glfw (Camilla Löwy)](https://github.com/glfw/glfw)
* [json (nlohmann)](https://github.com/nlohmann/json)
* [libad9361-iio (Analog Devices)](https://github.com/analogdevicesinc/libad9361-iio)
* [libairspy (Airspy)](https://github.com/airspy/airspyone_host)
* [libairspyhf (Airspy)](https://github.com/airspy/airspyhf)
* [libbladeRF (Nuand)](https://github.com/Nuand/bladeRF)
* [libcorrect (quiet)](https://github.com/quiet/libcorrect)
* [libfobos (RigExpert)](https://github.com/AlexandreRouma/libfobos)
* [libhackrf (Great Scott Gadgets)](https://github.com/greatscottgadgets/hackrf)
* [libhydrasdr (HydraSDR)](https://github.com/hydrasdr/hydrasdr-host)
* [libiio (Analog Devices)](https://github.com/analogdevicesinc/libiio)
* [libmirisdr-5 (ericek111)](https://github.com/ericek111/libmirisdr-5)
* [libogg (Xiph.Org)](https://github.com/xiph/ogg)
* [libperseus-sdr (Microtelecom)](https://github.com/bubnikv/libperseus-sdr)
* [librfnm (RFNM)](https://github.com/AlexandreRouma/librfnm)
* [librtlsdr (Osmocom)](https://github.com/osmocom/rtl-sdr)
* [libsddc (ik1xpv)](https://github.com/ik1xpv/ExtIO_sddc)
* [libusb (libusb.info)](https://github.com/libusb/libusb)
* [libxml2 (Daniel Veillard)](https://gitlab.gnome.org/GNOME/libxml2)
* [LimeSuite (MyriadRF)](https://github.com/myriadrf/LimeSuite)
* [logmmse (rajivpoddar)](https://github.com/rajivpoddar/logmmse)
* [Mbed TLS (Trusted Firmware)](https://github.com/Mbed-TLS/mbedtls)
* [Opus (Xiph.Org)](https://github.com/xiph/opus)
* [picohash (Kazuho Oku)](https://github.com/kazuho/picohash)
* [PortAudio (portaudio.com)](https://github.com/PortAudio/portaudio)
* [Portable File Dialogs](https://github.com/samhocevar/portable-file-dialogs)
* [pthreads4w (John E. Bossom)](https://sourceforge.net/projects/pthreads4w)
* [RapidJSON (Tencent)](https://github.com/Tencent/rapidjson)
* [RtAudio (Gary Scavone)](https://github.com/thestk/rtaudio)
* [SDRplay API (SDRplay)](https://www.sdrplay.com/api/)
* [sondedump (dbdexter-dev)](https://github.com/dbdexter-dev/sondedump)
* [spdlog (Gabi Melman)](https://github.com/gabime/spdlog)
* [stb (Sean Barrett)](https://github.com/nothings/stb)
* [VOLK (libvolk.org)](https://github.com/gnuradio/volk)
* [zlib (Jean-loup Gailly, Mark Adler)](https://github.com/madler/zlib)
* [Zstandard (Meta)](https://github.com/facebook/zstd)

## License

SDRIAK is licensed under the [GNU General Public License, version 3](license) and comes with no warranty. The notices of the bundled third-party software are in [root/res/licenses/THIRD_PARTY_NOTICES.txt](root/res/licenses/THIRD_PARTY_NOTICES.txt), and every release package includes both.
