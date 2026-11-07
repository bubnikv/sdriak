# SDRIAK — SDR software for the QRP Labs QMX transceiver

The fork is to support the QRP Labs QMX transceiver.
Currently remote reception using an [Android server app](https://github.com/bubnikv/qmxserver-android) on Android 7 and newer is supported.

In the future, direct USB connection to QMX transceiver is planned, including the Android build.

**Please do not report bugs in this fork to original author.**

**Report bugs in this fork on this page, in ISSUES.** 

Please see [upstream project page](https://github.com/AlexandreRouma/SDRPlusPlus) for the basic list of its features.

## Thanks / Credits

Thanks and due respect to:
 
* Original author, Alexandre Rouma, for his great [work](https://github.com/AlexandreRouma/SDRPlusPlus). Due credits go to all contributors in the upstream project. 
* qrp73 for the [SDRPP fork](https://github.com/qrp73/SDRPP). Adopted from it: the signal level meter with peak hold, the KiwiSDR map rendering (filled country polygons, day/night terminator) and KiwiSDR client improvements, WebSocket client improvements, manual/auto AGC, squelch hysteresis and hold, extended WAV support in the file source, additional FFT windows with unity-gain normalization, and FLAC and 24-bit recording.
* Sanny Sanoff for [SDR++Brown](https://github.com/sannysanoff/SDRPlusPlusBrown). Adopted from it: the KiwiSDR source and map, the WebSDR view, the WebSocket client, logMMSE noise reduction and the macOS CoreAudio sink.
* Hans Summers G0UPL [QRP Labs QMX Transceiver](https://qrp-labs.com/qmx).
* Lee Salzman for the [enet](https://github.com/lsalzman/enet) UDP networking library used for QMX streaming.

## Feedback

Found an issue? File an [issue](https://github.com/bubnikv/sdriak/issues).
