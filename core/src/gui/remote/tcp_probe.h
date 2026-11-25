#pragma once
// The remote browser's Test for plain TCP servers without a richer probe
// (rtl_tcp, RFspace, SDRIAK Server): connect, optionally wait for the first
// bytes the server sends unasked, close.

#include "registry.h"

#include <atomic>

namespace remote {

    // expectBytes > 0: also wait up to 3 s for that many bytes (rtl_tcp
    // sends its 12-byte "RTL0" header on accept). Worker thread.
    ProbeResult probeTcp(const RemoteEntry& entry, const std::atomic<bool>& cancel, int expectBytes = 0);

}
