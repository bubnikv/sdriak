#pragma once
// The remote-receiver browser: map and list of every networked receiver
// the directories and the recent list know, with filters, Test and
// Connect. Plan: doc/todo/remote-receiver-browser.md.

#include "geomap.h"
#include "registry.h"
#include "remote_entry.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class ConfigManager;

namespace remote {

    class RemoteBrowser {
    public:
        using PickAction = std::function<void(const RemoteEntry&)>;

        // config/configPrefix hold the map position and the filters.
        RemoteBrowser(ConfigManager* config, std::string configPrefix, std::string title);

        // Open on the next draw(); onlyKind shows that kind alone until the
        // next open.
        void open(std::optional<RemoteKind> onlyKind = std::nullopt);
        // Every frame, at the same place in the ID stack.
        void draw();

        // Replace Connect with another action, e.g. websdr_view's "Add".
        // The action runs on the UI thread and the browser closes after it.
        void setPickAction(std::string label, PickAction action);

    private:
        struct Group {
            std::string key;          // host key
            std::vector<int> members; // indices into m_entries, native first
            std::optional<ImVec2> cart; // map position
        };

        struct Filters {
            std::set<RemoteKind> hiddenKinds;
            bool coversFrequency = false;
            bool freeOnly = false;
            bool hideFull = true;
            bool hideUnconnectable = false;
            bool fullControlOnly = false;
            bool iqOnly = false;
            bool waterfallOnly = false;
            bool favouritesOnly = false;
            bool extApiOnly = false;
        };

        void rebuild();
        void loadFilters();
        void saveFilters();
        bool kindShown(RemoteKind kind) const;
        // Cached per frame by updateVisibility() for entries in m_entries.
        bool entryVisible(const RemoteEntry& e) const;
        bool computeVisible(const RemoteEntry& e) const;
        void updateVisibility();
        // First visible member, or -1.
        int groupLead(const Group& g) const;
        const RemoteEntry* selectedEntry() const;
        void selectGroup(int groupIndex);
        void selectEntry(int entryIndex);

        // Map view.
        void drawMap(float height, bool fullscreenPopup);
        void drawMarkers();
        std::vector<int> groupsAtScreenPos(ImVec2 screenPos) const;
        void handleMapClick();
        void drawClusterPicker();
        void drawMarkerShape(ImDrawList* dl, RemoteKind kind, ImVec2 c, float r, ImU32 fill, ImU32 outline, float thickness) const;
        ImU32 stateColor(const RemoteEntry& e) const;

        // List view.
        void drawList(float height);

        void drawTopRow();
        void drawFiltersPopup();
        void drawPanel(bool overlay);
        void drawFooter(bool desktopPopup, bool fullscreenPopup);
        void doPick(const RemoteEntry& e);
        std::optional<std::pair<double, double>> qthLatLon() const;

        ConfigManager* m_config;
        const std::string m_prefix;
        const std::string m_title;
        std::string m_pickLabel = "Connect";
        PickAction m_pickAction;

        geomap::GeoMap m_geoMap;
        std::vector<RemoteEntry> m_entries;
        std::vector<uint8_t> m_visible; // per m_entries, this frame
        std::vector<Group> m_groups;
        std::map<std::string, int> m_groupByKey;
        uint64_t m_dirGeneration = 0;
        uint64_t m_recentGeneration = 0;

        Filters m_filters;
        std::optional<RemoteKind> m_onlyKind;
        bool m_pendingOpen = false;
        bool m_listView = false;
        char m_search[128] = "";
        char m_qth[16] = "";
        std::string m_selectedGroup;   // host key
        std::string m_selectedEntryId;

        struct ClusterPick {
            std::string label;
            std::string groupKey;
        };
        std::vector<ClusterPick> m_clusterPicks;

        // List sort: column id and direction, and the sorted visible rows.
        std::vector<int> m_rows;
        bool m_rowsDirty = true;

        bool m_maximized = false;
        bool m_restoreValid = false;
        ImVec2 m_restorePos = ImVec2(0.0f, 0.0f);
        ImVec2 m_restoreSize = ImVec2(0.0f, 0.0f);
    };

    // The shared browser, drawn by the main window: the Source menu opens it
    // with every kind, each network source's "Browse..." with its own kind.
    void openBrowser(std::optional<RemoteKind> onlyKind = std::nullopt);
    void drawBrowser();

    // Connect to an entry through its kind's handler: stop the radio, select
    // the source, hand the entry to the module, remember it in Recent.
    // UI thread. False when the kind has no handler.
    bool connectEntry(const RemoteEntry& entry);

    // Core shutdown, before modules end: stop directory fetches and probes.
    void shutdown();

}
