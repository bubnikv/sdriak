#pragma once
// Recent and favourite receivers of every kind, in core config under
// "remoteReceivers". Covers the sources without a public directory.

#include "remote_entry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace remote {

    // Identity across providers: kind + lower-case host:port + path.
    std::string entryKey(const RemoteEntry& entry);

    // Favourites first, then recent, most recent first; favourite set on
    // the favourites. UI thread.
    std::vector<RemoteEntry> recentEntries();
    // Remember a connection. Source modules call it for connections made
    // from their own menus; the browser calls it on Connect.
    void recordRecent(const RemoteEntry& entry);
    void setFavourite(const RemoteEntry& entry, bool favourite);
    void forgetRecent(const RemoteEntry& entry);
    // Bumped on every change.
    uint64_t recentGeneration();

    // An entry for a receiver typed into a source menu.
    RemoteEntry manualEntry(RemoteKind kind, const RemoteTarget& target, const std::string& name = {});

}
