/*
 * This file is based on code from project SDRPlusPlusBrown (https://github.com/sannysanoff/SDRPlusPlusBrown)
 * originally licensed under the GNU General Public License, version 3 (GPLv3).
 *
 * Modified and extended by qrp73, 2025 (https://github.com/qrp73/SDRPP).
 * Modified for SDRIAK, 2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "kiwisdr_directory.h"
#include <gui/brown/geomap.h>

class ConfigManager;

struct KiwiSDRMapSelector {
    // How server markers are rendered on the map.
    enum class MarkerStyle {
        HideFull,        // original: full servers hidden, fill from SNR
        StatusColored,   // full = red, extended freq range = violet, else SNR
    };

    KiwiSDRMapSelector(const std::string& root, ConfigManager* config, const std::string& configPrefix);

    void openPopup();
    void drawPopup(std::function<void(const std::string&, const std::string&, const std::optional<ServerEntry::FrequencyBand>&)> onSelected);

    // Runtime-toggleable. Default keeps the original behavior so existing
    // users see no surprise; a popup button cycles between styles.
    MarkerStyle markerStyle = MarkerStyle::HideFull;

private:
    bool shouldShowServer(const ServerEntry& server) const;
    // True if `server` currently passes every filter drawMarkers() uses to
    // decide whether to paint it. Hit-testing shares this so a click can
    // never select a marker that isn't on screen.
    bool isServerVisible(const ServerEntry& server) const;
    // Indices of every visible marker whose drawn square contains the
    // absolute screen position, topmost (highest z / last-drawn) first.
    // Shared by the click handler and the overlay hit-test predicate handed
    // to GeoMap::draw so the two can't disagree.
    std::vector<int> markersAtScreenPos(ImVec2 screenPos) const;
    // Convenience: the topmost hit from markersAtScreenPos, or -1.
    int markerIndexAtScreenPos(ImVec2 screenPos) const;
    // Marks server `i` selected (clearing the rest) and moves it to the back
    // so it renders on top with the highlight. Overloads by unique URL.
    void selectServerIndex(int i);
    void selectServerByUrl(const std::string& url);
    void drawMarkers();
    void handleHitTest();
    // The selected+visible server, or nullptr. Used both by the info panel
    // and by the phone-landscape footer that hosts the Test button.
    const ServerEntry* selectedServer() const;
    // `showTestButton` draws the "Test server" action inline under the info.
    // On small (Android phone-landscape) screens it's false and the action
    // moves to the fixed footer instead, so a long info list can't push it
    // below the bottom edge.
    void drawSelectionPanel(bool showTestButton);
    // When a tap lands on several overlapping markers, we can't guess which
    // the user meant, so we pop a small "pick one" list instead of silently
    // choosing the topmost. Candidates are held by URL (a stable id) because
    // `servers` gets reordered by selection between frames.
    void drawClusterPicker();
    struct ClusterPick {
        std::string label;
        std::string url;
    };
    std::vector<ClusterPick> clusterPicks;

    geomap::GeoMap geoMap;
    std::vector<ServerEntry> servers;
    bool serversReady = false;
    bool showExtApiOnly = false;
    bool mapPopupMaximized = false;
    bool mapPopupRestoreValid = false;
    ImVec2 mapPopupRestorePos = ImVec2(0.0f, 0.0f);
    ImVec2 mapPopupRestoreSize = ImVec2(0.0f, 0.0f);
    const std::string configPrefix;
    ConfigManager* config = nullptr;

    KiwiSDRDirectoryClient directory;
    KiwiSDRTester tester;
};
