#pragma once
// The built-in directory providers and their parsers. The parsers are
// separate so tests can feed them saved documents without the network.

#include "directory.h"

#include <memory>
#include <string>
#include <vector>

namespace remote {

    // KiwiSDR: http://rx.linkfanel.net/kiwisdr_com.js, a JS literal.
    std::unique_ptr<DirectoryProvider> makeKiwiSDRProvider();
    std::vector<RemoteEntry> parseKiwiSDRDirectory(const std::string& body);

    // Web-888: https://www.rx-888.com/api/devices, {devices[]} in the
    // kiwisdr.com field format. The receivers speak the KiwiSDR protocol.
    std::unique_ptr<DirectoryProvider> makeRx888Provider();
    std::vector<RemoteEntry> parseRx888Directory(const std::string& body);

    // sdr-list.xyz: https://sdr-list.xyz/api/v1/receivers, a JSON array.
    // Only PhantomSDR-Plus receivers, as KiwiSDR kind (their KiwiSDR
    // bridge); entries not seen for a day are dropped.
    std::unique_ptr<DirectoryProvider> makeSdrListProvider();
    std::vector<RemoteEntry> parseSdrListDirectory(const std::string& body, int64_t fetchedUnix);

    // SpyServer: https://airspy.com/directory/status.json, {servers[]}.
    std::unique_ptr<DirectoryProvider> makeSpyServerProvider();
    std::vector<RemoteEntry> parseSpyServerDirectory(const std::string& body, int64_t fetchedUnix);

    // UberSDR: https://instances.ubersdr.org/api/instances, {count, instances[]}.
    std::unique_ptr<DirectoryProvider> makeUberSDRProvider();
    std::vector<RemoteEntry> parseUberSDRDirectory(const std::string& body, int64_t fetchedUnix);

    // Receiverbook: https://www.receiverbook.de/map, HTML around a
    // `var receivers = [...];` literal. OpenWebRX and KiwiSDR receivers;
    // WebSDR ones are skipped.
    std::unique_ptr<DirectoryProvider> makeReceiverbookProvider();
    std::vector<RemoteEntry> parseReceiverbook(const std::string& body);

    // OpenWebRX+ versions with the iq mode: 1.2.129 and later.
    bool owrxVersionHasIq(const std::string& version);

}
