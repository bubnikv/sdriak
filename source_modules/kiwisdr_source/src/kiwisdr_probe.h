#pragma once
#include <atomic>

#include <gui/remote/registry.h>

// The remote browser's Test for a KiwiSDR: SND until the first IQ packet.
// Worker thread.
remote::ProbeResult probeKiwiSDR(const remote::RemoteEntry& entry, const std::atomic<bool>& cancel);
