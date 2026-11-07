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
#define IMGUI_DEFINE_MATH_OPERATORS
#ifdef _WIN32
#define _WINSOCKAPI_ // stops windows.h including winsock.h
#endif

#include "kiwisdr_map.h"

#include <cmath>
#include <cstdio>
#include <sstream>

#include <core.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <gui/style.h>
#include "gui/widgets/simple_widgets.h"


namespace {

    // Compact MHz range, e.g. "0-30 MHz" or "0.015-32 MHz". The directory
    // gives raw Hz integers, which are hard to read at a glance. %g trims
    // trailing zeros so round bands stay short.
    std::string formatBandMHz(const ServerEntry::FrequencyBand& band) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g-%g MHz",
                      band.startHz / 1e6, band.endHz / 1e6);
        return std::string(buf);
    }

    ImColor markerFillForSnr(float maxSnr) {
        if (maxSnr > 22) return ImColor(0.0f, 1.0f, 0.0f);
        if (maxSnr > 12) return ImColor(0.6f, 0.6f, 0.6f);
        return ImColor(0.3f, 0.3f, 0.3f);
    }

    bool shouldUseFullscreenPopup(ImVec2 displaySize) {
        const float fontSize = ImGui::GetFontSize();
        return displaySize.x < fontSize * 56.0f || displaySize.y < fontSize * 36.0f;
    }

    ImVec2 desktopPopupMinSize(ImVec2 displaySize) {
        const float fontSize = ImGui::GetFontSize();
        return ImVec2(
            ImMin(displaySize.x, ImMax(320.0f, fontSize * 24.0f)),
            ImMin(displaySize.y, ImMax(240.0f, fontSize * 18.0f))
        );
    }

}


KiwiSDRMapSelector::KiwiSDRMapSelector(const std::string& root, ConfigManager* config, const std::string& configPrefix)
    : configPrefix(configPrefix), config(config), directory(root) {
    json def = json({});
    config->load(def);
    geoMap.loadFrom(*config, configPrefix.c_str()); // configPrefix is like "mapselector1_"
}

void KiwiSDRMapSelector::openPopup() {
    ImGui::OpenPopup((configPrefix + ": The KiwiSDR Map").c_str());
}

void KiwiSDRMapSelector::drawPopup(std::function<void(const std::string&, const std::string&, const std::optional<ServerEntry::FrequencyBand>&)> onSelected) {
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
#ifdef __ANDROID__
    const bool desktopPopup = false;
    const bool fullscreenPopup = shouldUseFullscreenPopup(displaySize);
#else
    const bool desktopPopup = true;
    const bool fullscreenPopup = false;
#endif
    const std::string popupTitle = configPrefix + ": The KiwiSDR Map";
    const bool lockedFullscreenPopup = fullscreenPopup || (desktopPopup && mapPopupMaximized);
    const ImVec2 popupPos = lockedFullscreenPopup ? ImVec2(0.0f, 0.0f) : displaySize * 0.125f;
    const ImVec2 popupSize = lockedFullscreenPopup ? displaySize : displaySize * 0.75f;
    ImGuiWindowFlags popupFlags = 0;
    if (!desktopPopup) {
        popupFlags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    }
    if (lockedFullscreenPopup) {
        popupFlags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    }
    if (fullscreenPopup) {
        popupFlags |= ImGuiWindowFlags_NoTitleBar;
    }

    if (desktopPopup && !mapPopupMaximized) {
        ImGui::SetNextWindowSizeConstraints(desktopPopupMinSize(displaySize), displaySize);
    }

    const ImGuiCond popupPlacementCond = lockedFullscreenPopup ? ImGuiCond_Always : (desktopPopup ? ImGuiCond_FirstUseEver : ImGuiCond_Always);
    ImGui::SetNextWindowPos(popupPos, popupPlacementCond);
    ImGui::SetNextWindowSize(popupSize, popupPlacementCond);
    if (!ImGui::BeginPopupModal(popupTitle.c_str(), nullptr, popupFlags)) {
        return;
    }

    const ImVec2 contentSize = ImGui::GetContentRegionAvail();
    const float footerHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    // NoScrollbar/NoScrollWithMouse: the selection-panel labels (server URL
    // etc.) can overflow the child, but the map must never scroll — panning
    // belongs to the map's own drag handling, and the Android drag-scroll
    // recognizer also leaves NoScrollWithMouse windows alone.
    ImGui::BeginChild("##geomap-kiwisdr", ImVec2(contentSize.x, ImMax(1.0f, contentSize.y - footerHeight)), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const char* filterButtonLabel = showExtApiOnly ? "Show all stations" : "Show EXT API only";
    // Bump button alpha for the whole map control row (Zoom/Reset/filter
    // submitted inside geoMap.draw, plus the "Hide full" button on the
    // same line below) — those buttons sit over the map and the default
    // theme alpha makes them hard to read against the country fills.
    pushOverlayButtonStyle();
    // Markers go through geoMap.draw's overlay slot so they paint above
    // the country fills but below the Zoom/Reset/filter button row in
    // the same child window. Drawing them after geoMap.draw returns
    // would put marker rectangles on top of the buttons.
    geoMap.draw(
        filterButtonLabel,
        [this]() { showExtApiOnly = !showExtApiOnly; },
        [this]() {
            if (serversReady) drawMarkers();
        },
        // Overlay hit-test: lets GeoMap suppress the country tooltip and the
        // drag-to-pan start when the pointer is over a marker, so a click on
        // a marker is owned by our selection handler instead of leaking
        // through to the map. Shares markerIndexAtScreenPos with
        // handleHitTest so the two always agree on what counts as a hit.
        [this]() -> bool {
            return serversReady && markerIndexAtScreenPos(ImGui::GetMousePos()) >= 0;
        });
    if (geoMap.scaleTranslateDirty) {
        geoMap.saveTo(*config, configPrefix.c_str());
        geoMap.scaleTranslateDirty = false;
    }

    // Marker rendering toggle. Label shows the action — clicking switches
    // to the other mode.
    ImGui::SameLine();
    const char* markerStyleLabel = (markerStyle == MarkerStyle::StatusColored)
        ? "Hide full servers" : "Show full as red";
    if (ImGui::Button(markerStyleLabel)) {
        markerStyle = (markerStyle == MarkerStyle::StatusColored)
            ? MarkerStyle::HideFull : MarkerStyle::StatusColored;
    }
    popOverlayButtonStyle();

    directory.requestRefresh();
    if (auto fresh = directory.takeIfReady()) {
        servers = std::move(*fresh);
        serversReady = true;
    }

    const std::string error = directory.errorMessage();
    if (!serversReady) {
        if (!error.empty()) {
            ImGui::Text("%s", error.c_str());
        }
        else {
            ImGui::Text("Loading KiwiSDR servers list..");
        }
    }
    else {
        ImGui::Text("%d KiwiSDR servers", (int)servers.size());
        // Markers are drawn from inside geoMap.draw via the overlay slot
        // so they sit under the button row. handleHitTest is input-only
        // and runs here.
        handleHitTest();
        drawClusterPicker();
        // On phone-landscape (fullscreenPopup) the Test action moves to the
        // fixed footer below, so the variable-length info list can't shove it
        // off the short bottom edge; elsewhere it stays inline under the info.
        drawSelectionPanel(!fullscreenPopup);

        const std::string testStatus = tester.statusText();
        const std::string testError = tester.errorText();
        if (!testStatus.empty()) {
            ImGui::Text("%s", testStatus.c_str());
        }
        if (!testError.empty()) {
            ImGui::Text("Server test error: %s", testError.c_str());
        }
    }

    ImGui::EndChild();

    if (desktopPopup) {
        if (ImGui::Button(mapPopupMaximized ? "Restore" : "Maximize")) {
            if (mapPopupMaximized) {
                mapPopupMaximized = false;
                if (mapPopupRestoreValid) {
                    ImGui::SetWindowPos(mapPopupRestorePos, ImGuiCond_Always);
                    ImGui::SetWindowSize(mapPopupRestoreSize, ImGuiCond_Always);
                }
            }
            else {
                mapPopupRestorePos = ImGui::GetWindowPos();
                mapPopupRestoreSize = ImGui::GetWindowSize();
                mapPopupRestoreValid = true;
                mapPopupMaximized = true;
                ImGui::SetWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetWindowSize(displaySize, ImGuiCond_Always);
            }
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    // Phone-landscape: the Test button lives here in the fixed footer (not in
    // the scrolling-off info panel) so it's always reachable at the bottom
    // edge. Same rule that drives fullscreenPopup / the forced landscape.
    if (fullscreenPopup) {
        if (const ServerEntry* sel = selectedServer()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(tester.isInProgress());
            const bool doTest = ImGui::Button("Test server");
            ImGui::EndDisabled();
            if (doTest) {
                tester.start(sel->url, sel->loc, sel->band);
            }
        }
    }
    if (auto ok = tester.lastOk()) {
        ImGui::SameLine();
        if (ImGui::Button(("Use tested server: " + ok->hostPort).c_str())) {
            onSelected(ok->hostPort, ok->loc, ok->band);
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::EndPopup();
}

bool KiwiSDRMapSelector::shouldShowServer(const ServerEntry& server) const {
    return !showExtApiOnly || server.extApi > 0;
}

bool KiwiSDRMapSelector::isServerVisible(const ServerEntry& server) const {
    if (!shouldShowServer(server)) return false;
    // HideFull mode drops fully-occupied servers. Hit-testing must honor the
    // exact same rule — otherwise a full server, invisible on screen, would
    // still be clickable (and, sitting at a higher z-index than the visible
    // marker beside it, would even win the pick).
    const bool isFull = server.users >= server.usersmax;
    if (markerStyle == MarkerStyle::HideFull && isFull) return false;
    return true;
}

std::vector<int> KiwiSDRMapSelector::markersAtScreenPos(ImVec2 screenPos) const {
    std::vector<int> hits;
    if (!geoMap.recentMapToScreen) return hits;
    const float sz = style::baseFont->FontSize;
    // Test the same square the marker is drawn as (half-extent sz/2), so the
    // clickable area matches the visible one exactly. On touch, widen by an
    // extra half-marker so a fat finger still lands the tap; on desktop the
    // mouse hits precisely what it sees, keeping pan-next-to-a-marker usable.
#ifdef __ANDROID__
    const float half = sz;
#else
    const float half = sz / 2.0f;
#endif
    const ImVec2 rel = screenPos - geoMap.recentCanvasPos;
    // Markers paint front-to-back, so the highest-index marker covering the
    // point is the one on top. Scan back-to-front so hits come out topmost
    // first: selection order then matches render order.
    for (int i = (int)servers.size() - 1; i >= 0; i--) {
        const auto& s = servers[i];
        if (!isServerVisible(s)) continue;
        const ImVec2 dest = geoMap.recentMapToScreen(s.gps);
        if (std::fabs(dest.x - rel.x) <= half && std::fabs(dest.y - rel.y) <= half) {
            hits.push_back(i);
        }
    }
    return hits;
}

int KiwiSDRMapSelector::markerIndexAtScreenPos(ImVec2 screenPos) const {
    const auto hits = markersAtScreenPos(screenPos);
    return hits.empty() ? -1 : hits.front();
}

void KiwiSDRMapSelector::selectServerIndex(int i) {
    if (i < 0 || i >= (int)servers.size()) return;
    for (auto& s : servers) s.selected = false;
    ServerEntry chosen = servers[i];
    chosen.selected = true;
    servers.erase(servers.begin() + i);
    servers.emplace_back(std::move(chosen));
}

void KiwiSDRMapSelector::selectServerByUrl(const std::string& url) {
    for (int i = 0; i < (int)servers.size(); i++) {
        if (servers[i].url == url) {
            selectServerIndex(i);
            return;
        }
    }
}

void KiwiSDRMapSelector::drawMarkers() {
    const auto sz = style::baseFont->FontSize;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
#ifndef __ANDROID__
    // Hover-tooltip is desktop-only: on a touch screen the cursor only
    // exists while a finger is down, the finger occludes the tooltip,
    // and the tap is already routed to the click-to-select handler.
    // Touch users discover server details through the selection panel.
    //
    // Gate: cursor in this child window AND not over a registered widget.
    // Without the !IsAnyItemHovered() check, the marker tooltip would
    // show through the Zoom/Reset/filter buttons sitting in the same
    // child window.
    const bool windowHovered = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
    const ImVec2 mousePos = ImGui::GetMousePos();
    const ServerEntry* hovered = nullptr;
#endif

    for (const auto& s : servers) {
        if (!isServerVisible(s)) continue;
        const bool isFull = s.users >= s.usersmax;

        // Fill: SNR-based by default; in StatusColored mode, extended-freq
        // servers (>32 MHz, i.e. usable for VHF/FM) are violet, and full
        // servers are red — overrides applied in priority order.
        ImColor fill = markerFillForSnr(s.maxSnr);
        if (markerStyle == MarkerStyle::StatusColored) {
            if (s.band && s.band->endHz > 32000000ull) {
                fill = ImColor(0.6f, 0.4f, 1.0f);
            }
            if (isFull) {
                fill = ImColor(0.8f, 0.0f, 0.0f);
            }
        }

        const auto dest = geoMap.recentMapToScreen(s.gps);
        const auto p0 = geoMap.recentCanvasPos + dest - ImVec2(sz / 2, sz / 2);
        const auto p1 = geoMap.recentCanvasPos + dest + ImVec2(sz / 2, sz / 2);
        drawList->AddRectFilled(p0, p1, fill, sz / 4.0f);
        if (s.selected) {
            drawList->AddRect(p0, p1, ImColor(1.0f, 1.0f, 0.0f), sz / 4.0f);
            drawList->AddRect(p0 + ImVec2(1, 1), p1 - ImVec2(1, 1), ImColor(1.0f, 1.0f, 0.0f), sz / 4.0f);
        }
        else {
            drawList->AddRect(p0, p1, ImColor(0.0f, 0.0f, 0.0f), sz / 4.0f);
        }
#ifndef __ANDROID__
        // Track the topmost (last-drawn) marker under the cursor so the
        // tooltip below picks the same server the click handler would.
        if (windowHovered &&
            mousePos.x >= p0.x && mousePos.x <= p1.x &&
            mousePos.y >= p0.y && mousePos.y <= p1.y) {
            hovered = &s;
        }
#endif
    }

#ifndef __ANDROID__
    if (hovered) {
        // Brief tooltip — full details are in the selection panel after click.
        std::ostringstream ss;
        ss << hovered->name << "\n"
           << "Loc: " << hovered->loc;
        if (hovered->band) {
            ss << "\nBand: " << formatBandMHz(*hovered->band);
        }
        ImGui::SetTooltip("%s", ss.str().c_str());
    }
#endif
}

void KiwiSDRMapSelector::handleHitTest() {
    if (!(ImGui::IsMouseClicked(0) && ImGui::GetMouseClickedCount(0) == 1)) return;
    // IsMouseClicked is a global query — without this gate, clicking the
    // Zoom/Reset/filter buttons sitting in the same child window would
    // also pick whatever marker happens to lie under the button rect.
    // AllowWhenBlockedByActiveItem is kept as a belt-and-suspenders guard:
    // GeoMap now refuses to start a pan (and take ActiveID) on a click that
    // lands on a marker, so on a real marker click nothing blocks hover —
    // but the flag keeps us robust if some other widget holds ActiveID.
    // The IsAnyItemHovered check still rejects clicks on a real widget.
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        ImGui::IsAnyItemHovered()) return;
    const auto hits = markersAtScreenPos(ImGui::GetMousePos());
    if (hits.empty()) return;
    if (hits.size() == 1) {
        selectServerIndex(hits.front());
        return;
    }
    // Overlapping cluster: don't guess. Capture the candidates by URL and
    // open a picker (drawn in drawClusterPicker). hits is topmost-first, so
    // the list reads top-of-stack downward.
    clusterPicks.clear();
    for (int idx : hits) {
        clusterPicks.push_back({ servers[idx].name, servers[idx].url });
    }
    ImGui::OpenPopup("##kiwi-cluster-pick");
}

void KiwiSDRMapSelector::drawClusterPicker() {
    if (!ImGui::BeginPopup("##kiwi-cluster-pick", ImGuiWindowFlags_NoMove)) return;
    ImGui::TextUnformatted("Multiple stations here - pick one:");
    // Cap the visible height so a dense cluster (e.g. central Europe) scrolls
    // instead of running off the screen; zooming in still thins the stack.
    const float rowH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    const bool scroll = clusterPicks.size() > 8;
    if (scroll) ImGui::BeginChild("##cluster-scroll", ImVec2(0.0f, rowH * 8.0f), false);
    for (size_t k = 0; k < clusterPicks.size(); k++) {
        const auto& c = clusterPicks[k];
        // Suffix a unique id so duplicate names still get distinct buttons.
        if (ImGui::Button((c.label + "##pick" + std::to_string(k)).c_str())) {
            selectServerByUrl(c.url);
            ImGui::CloseCurrentPopup();
        }
    }
    if (scroll) ImGui::EndChild();
    ImGui::EndPopup();
}

const ServerEntry* KiwiSDRMapSelector::selectedServer() const {
    for (const auto& s : servers) {
        if (s.selected && shouldShowServer(s)) return &s;
    }
    return nullptr;
}

void KiwiSDRMapSelector::drawSelectionPanel(bool showTestButton) {
    for (const auto& s : servers) {
        if (!s.selected || !shouldShowServer(s)) continue;
        // doOverlayText puts a translucent dark backdrop behind each label
        // so the text stays legible over the colored country fills.
        doOverlayText("%s", s.name.c_str());
        doOverlayText("%s", s.loc.c_str());
        if (s.band) {
            doOverlayText("Band: %s", formatBandMHz(*s.band).c_str());
        }
        if (!s.antenna.empty()) {
            doOverlayText("ANT: %s", s.antenna.c_str());
        }
        if (!s.sdrHardware.empty()) {
            doOverlayText("HW: %s", s.sdrHardware.c_str());
        }
        if (!s.swVersion.empty()) {
            doOverlayText("VER: %s", s.swVersion.c_str());
        }
        if (!s.qth.empty()) {
            doOverlayText("QTH: %s", s.qth.c_str());
        }
        if (s.maxSnr > 0) {
            doOverlayText("SNR: %d", (int)s.maxSnr);
        }
        if (s.usersmax > 0) {
            doOverlayText("USR: %d/%d", s.users, s.usersmax);
        }
        doOverlayText("EXT API: %d", s.extApi);
        doOverlayText("URL: %s", s.url.c_str());

        // On phone-landscape the Test action is drawn in the fixed footer
        // instead (see drawPopup), so skip it here.
        if (showTestButton) {
            ImGui::BeginDisabled(tester.isInProgress());
            const bool doTest = ImGui::Button("Test server");
            ImGui::EndDisabled();
            if (doTest) {
                tester.start(s.url, s.loc, s.band);
            }
        }
    }
}
