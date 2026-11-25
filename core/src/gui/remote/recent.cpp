#include "recent.h"

#include <algorithm>
#include <atomic>
#include <cctype>

#include <core.h>

namespace remote {

    namespace {
        constexpr size_t kRecentMax = 30;
        constexpr const char* kConfigKey = "remoteReceivers";
        std::atomic<uint64_t> g_generation{ 1 };

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        // What is stored: the entry without volatile directory state.
        RemoteEntry storable(RemoteEntry e) {
            e.users.reset();
            e.usersMax.reset();
            e.lastSeenUnix.reset();
            e.favourite = false;
            if (e.provider != "recent") { e.id = "recent/" + entryKey(e); }
            e.provider = "recent";
            return e;
        }

        std::vector<RemoteEntry> readList(const nlohmann::json& arr) {
            std::vector<RemoteEntry> out;
            if (!arr.is_array()) { return out; }
            for (const auto& j : arr) {
                if (auto e = entryFromJson(j)) { out.push_back(std::move(*e)); }
            }
            return out;
        }

        nlohmann::json writeList(const std::vector<RemoteEntry>& list) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& e : list) { arr.push_back(entryToJson(e)); }
            return arr;
        }

        void load(std::vector<RemoteEntry>& recent, std::vector<RemoteEntry>& favourites) {
            nlohmann::json section;
            {
                auto cfg = core::configManager.read();
                section = cfg.value(kConfigKey, nlohmann::json::object());
            }
            if (!section.is_object()) { return; }
            recent = readList(section.value("recent", nlohmann::json::array()));
            favourites = readList(section.value("favourites", nlohmann::json::array()));
        }

        void save(const std::vector<RemoteEntry>& recent, const std::vector<RemoteEntry>& favourites) {
            nlohmann::json section = nlohmann::json::object();
            section["recent"] = writeList(recent);
            section["favourites"] = writeList(favourites);
            core::configManager.edit().set(kConfigKey, section);
            g_generation++;
        }

        void removeKey(std::vector<RemoteEntry>& list, const std::string& key) {
            list.erase(std::remove_if(list.begin(), list.end(), [&](const RemoteEntry& e) { return entryKey(e) == key; }), list.end());
        }
    }

    std::string entryKey(const RemoteEntry& e) {
        return std::string(kindKey(e.kind)) + "/" + lower(e.target.hostPort()) + lower(e.target.path);
    }

    std::vector<RemoteEntry> recentEntries() {
        std::vector<RemoteEntry> recent, favourites;
        load(recent, favourites);
        std::vector<RemoteEntry> out;
        for (auto& f : favourites) {
            f.favourite = true;
            removeKey(recent, entryKey(f));
            out.push_back(std::move(f));
        }
        for (auto& r : recent) { out.push_back(std::move(r)); }
        return out;
    }

    void recordRecent(const RemoteEntry& entry) {
        std::vector<RemoteEntry> recent, favourites;
        load(recent, favourites);
        const std::string key = entryKey(entry);
        RemoteEntry stored = storable(entry);
        // A module reporting its own connection knows less than the browser
        // entry it may have come from: keep what the earlier record knew.
        if (entry.provider == "recent") {
            for (const auto& old : recent) {
                if (entryKey(old) != key) { continue; }
                if (!old.name.empty()) { stored.name = old.name; }
                if (stored.location.empty()) { stored.location = old.location; }
                if (!stored.latLon) { stored.latLon = old.latLon; }
                if (stored.ranges.empty()) { stored.ranges = old.ranges; }
                if (stored.details.empty()) { stored.details = old.details; }
                if (stored.flags.empty()) { stored.flags = old.flags; }
                if (!stored.tlsRequired) { stored.tlsRequired = old.tlsRequired; }
                break;
            }
        }
        removeKey(recent, key);
        recent.insert(recent.begin(), std::move(stored));
        if (recent.size() > kRecentMax) { recent.resize(kRecentMax); }
        save(recent, favourites);
    }

    void setFavourite(const RemoteEntry& entry, bool favourite) {
        std::vector<RemoteEntry> recent, favourites;
        load(recent, favourites);
        const std::string key = entryKey(entry);
        removeKey(favourites, key);
        if (favourite) { favourites.push_back(storable(entry)); }
        save(recent, favourites);
    }

    void forgetRecent(const RemoteEntry& entry) {
        std::vector<RemoteEntry> recent, favourites;
        load(recent, favourites);
        const std::string key = entryKey(entry);
        removeKey(recent, key);
        removeKey(favourites, key);
        save(recent, favourites);
    }

    uint64_t recentGeneration() {
        return g_generation.load();
    }

    RemoteEntry manualEntry(RemoteKind kind, const RemoteTarget& target, const std::string& name) {
        RemoteEntry e;
        e.kind = kind;
        e.target = target;
        e.provider = "recent";
        e.name = name.empty() ? target.hostPort() : name;
        e.id = "recent/" + entryKey(e);
        return e;
    }

}
