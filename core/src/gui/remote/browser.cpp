/*
 * The map view is based on the KiwiSDR map selector from project
 * SDRPlusPlusBrown (https://github.com/sannysanoff/SDRPlusPlusBrown)
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

#include "browser.h"
#include "directory.h"
#include "recent.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>

#include <config.h>
#include <core.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <gui/menus/source.h>
#include <gui/style.h>
#include <gui/widgets/simple_widgets.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>

namespace remote {

    namespace {

        bool shouldUseFullscreenPopup(ImVec2 displaySize) {
            const float fontSize = ImGui::GetFontSize();
            return displaySize.x < fontSize * 56.0f || displaySize.y < fontSize * 36.0f;
        }

        ImVec2 desktopPopupMinSize(ImVec2 displaySize) {
            const float fontSize = ImGui::GetFontSize();
            return ImVec2(ImMin(displaySize.x, ImMax(320.0f, fontSize * 24.0f)),
                          ImMin(displaySize.y, ImMax(240.0f, fontSize * 18.0f)));
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        // Provider preference when two providers list the same receiver:
        // the kind's own directory first, then rx-888.com and sdr-list.xyz
        // (fewer fields than kiwisdr.com for the same receiver), Receiverbook
        // last.
        int providerRank(const RemoteEntry& e) {
            if (e.provider == "rx-888.com" || e.provider == "sdr-list.xyz") { return 1; }
            if (e.provider == "receiverbook.de") { return 2; }
            if (e.provider == "recent") { return 3; }
            return 0;
        }

        std::string formatAge(int64_t seconds) {
            if (seconds < 0) { seconds = 0; }
            if (seconds < 120) { return std::to_string(seconds) + " s"; }
            if (seconds < 7200) { return std::to_string(seconds / 60) + " min"; }
            if (seconds < 2 * 86400) { return std::to_string(seconds / 3600) + " h"; }
            return std::to_string(seconds / 86400) + " d";
        }

        // Kind glyph for lists and tooltips.
        const char* kindGlyph(RemoteKind kind) {
            switch (kind) {
            case RemoteKind::KiwiSDR: return "(o)";
            case RemoteKind::UberSDR: return "<>";
            case RemoteKind::SpyServer: return "/\\";
            case RemoteKind::OpenWebRX: return "[]";
            default: return "*";
            }
        }

        constexpr const char* kQthKey = "remoteBrowserQth";

        // List columns.
        enum Column { COL_NAME, COL_KIND, COL_LOCATION, COL_DISTANCE, COL_USERS, COL_RANGE, COL_SNR, COL_SEEN, COL_COUNT };
    }

    // =========================================================================
    // Construction, data

    RemoteBrowser::RemoteBrowser(ConfigManager* config, std::string configPrefix, std::string title)
        : m_config(config), m_prefix(std::move(configPrefix)), m_title(std::move(title)) {
        m_geoMap.loadFrom(*m_config, m_prefix.c_str());
        loadFilters();
        std::string qth = core::configManager.read().value(kQthKey, std::string());
        std::snprintf(m_qth, sizeof m_qth, "%s", qth.c_str());
    }

    void RemoteBrowser::setPickAction(std::string label, PickAction action) {
        m_pickLabel = std::move(label);
        m_pickAction = std::move(action);
    }

    void RemoteBrowser::open(std::optional<RemoteKind> onlyKind) {
        m_onlyKind = onlyKind;
        m_pendingOpen = true;
        m_rowsDirty = true;
    }

    void RemoteBrowser::loadFilters() {
        auto cfg = m_config->read();
        const std::string p = m_prefix + "_filter_";
        m_filters.coversFrequency = cfg.value(p + "covers", false);
        m_filters.freeOnly = cfg.value(p + "free", false);
        m_filters.hideFull = cfg.value(p + "hideFull", true);
        m_filters.hideUnconnectable = cfg.value(p + "connectable", false);
        m_filters.fullControlOnly = cfg.value(p + "fullControl", false);
        m_filters.iqOnly = cfg.value(p + "iq", false);
        m_filters.waterfallOnly = cfg.value(p + "waterfall", false);
        m_filters.favouritesOnly = cfg.value(p + "favourites", false);
        m_filters.extApiOnly = cfg.value(p + "extApi", false);
        m_listView = cfg.value(m_prefix + "_listView", false);
        const auto hidden = cfg.value(p + "hiddenKinds", nlohmann::json::array());
        m_filters.hiddenKinds.clear();
        if (hidden.is_array()) {
            for (const auto& k : hidden) {
                if (!k.is_string()) { continue; }
                if (auto kind = kindFromKey(k.get<std::string>())) { m_filters.hiddenKinds.insert(*kind); }
            }
        }
    }

    void RemoteBrowser::saveFilters() {
        auto cfg = m_config->edit();
        const std::string p = m_prefix + "_filter_";
        cfg.set(p + "covers", m_filters.coversFrequency);
        cfg.set(p + "free", m_filters.freeOnly);
        cfg.set(p + "hideFull", m_filters.hideFull);
        cfg.set(p + "connectable", m_filters.hideUnconnectable);
        cfg.set(p + "fullControl", m_filters.fullControlOnly);
        cfg.set(p + "iq", m_filters.iqOnly);
        cfg.set(p + "waterfall", m_filters.waterfallOnly);
        cfg.set(p + "favourites", m_filters.favouritesOnly);
        cfg.set(p + "extApi", m_filters.extApiOnly);
        cfg.set(m_prefix + "_listView", m_listView);
        nlohmann::json hidden = nlohmann::json::array();
        for (auto k : m_filters.hiddenKinds) { hidden.push_back(kindKey(k)); }
        cfg.set(p + "hiddenKinds", hidden);
        m_rowsDirty = true;
    }

    void RemoteBrowser::rebuild() {
        auto& dir = DirectoryService::instance();
        std::vector<RemoteEntry> all = dir.snapshot();
        std::vector<RemoteEntry> recent = recentEntries();

        // One entry per receiver: kind + host:port + path. The preferred
        // provider's entry wins; a missing position is borrowed.
        std::map<std::string, int> byKey;
        std::vector<RemoteEntry> merged;
        merged.reserve(all.size() + recent.size());
        auto add = [&](RemoteEntry e) {
            const std::string key = entryKey(e);
            auto it = byKey.find(key);
            if (it == byKey.end()) {
                byKey[key] = (int)merged.size();
                merged.push_back(std::move(e));
                return;
            }
            RemoteEntry& have = merged[it->second];
            const bool fav = have.favourite || e.favourite;
            if (providerRank(e) < providerRank(have)) {
                if (!e.latLon) { e.latLon = have.latLon; }
                if (e.location.empty()) { e.location = have.location; }
                have = std::move(e);
            }
            else if (!have.latLon && e.latLon) {
                have.latLon = e.latLon;
            }
            have.favourite = fav;
        };
        for (auto& e : all) { add(std::move(e)); }
        for (auto& e : recent) { add(std::move(e)); }
        m_entries = std::move(merged);

        // One marker per host, native protocol first.
        m_groups.clear();
        m_groupByKey.clear();
        for (int i = 0; i < (int)m_entries.size(); i++) {
            const std::string key = m_entries[i].target.hostKey();
            auto it = m_groupByKey.find(key);
            if (it == m_groupByKey.end()) {
                m_groupByKey[key] = (int)m_groups.size();
                Group g;
                g.key = key;
                m_groups.push_back(std::move(g));
                it = m_groupByKey.find(key);
            }
            m_groups[it->second].members.push_back(i);
        }
        for (auto& g : m_groups) {
            std::stable_sort(g.members.begin(), g.members.end(), [this](int a, int b) {
                return kindRank(m_entries[a].kind) < kindRank(m_entries[b].kind);
            });
            for (int m : g.members) {
                if (m_entries[m].latLon) {
                    g.cart = geomap::geoToCartesian({ m_entries[m].latLon->first, m_entries[m].latLon->second }).toImVec2();
                    break;
                }
            }
        }
        // The selected group paints last, on top.
        auto sel = m_groupByKey.find(m_selectedGroup);
        if (sel != m_groupByKey.end()) {
            Group g = m_groups[sel->second];
            m_groups.erase(m_groups.begin() + sel->second);
            m_groups.push_back(std::move(g));
            m_groupByKey.clear();
            for (int i = 0; i < (int)m_groups.size(); i++) { m_groupByKey[m_groups[i].key] = i; }
        }
        m_rowsDirty = true;
    }

    bool RemoteBrowser::kindShown(RemoteKind kind) const {
        if (m_onlyKind) { return kind == *m_onlyKind; }
        return m_filters.hiddenKinds.count(kind) == 0;
    }

    bool RemoteBrowser::entryVisible(const RemoteEntry& e) const {
        const ptrdiff_t i = &e - m_entries.data();
        if (i >= 0 && i < (ptrdiff_t)m_visible.size()) { return m_visible[(size_t)i] != 0; }
        return computeVisible(e);
    }

    void RemoteBrowser::updateVisibility() {
        m_visible.resize(m_entries.size());
        for (size_t i = 0; i < m_entries.size(); i++) { m_visible[i] = computeVisible(m_entries[i]) ? 1 : 0; }
    }

    bool RemoteBrowser::computeVisible(const RemoteEntry& e) const {
        if (!kindShown(e.kind)) { return false; }
        const auto& f = m_filters;
        if (f.hideFull && e.isFull()) { return false; }
        if (f.freeOnly) {
            if (!e.users || !e.usersMax || *e.users >= *e.usersMax) { return false; }
        }
        if (f.coversFrequency && !e.ranges.empty() && !e.covers((uint64_t)std::max<double>(0.0, (double)gui::freqSelect.frequency))) {
            return false;
        }
        if (f.hideUnconnectable && !sigpath::remoteRegistry.unavailableReason(e).empty()) { return false; }
        if (f.fullControlOnly && e.kind == RemoteKind::SpyServer && !e.hasFlag("full-control")) { return false; }
        if (f.iqOnly && e.hasFlag("audio-only")) { return false; }
        if (f.waterfallOnly && !e.hasFlag("waterfall")) { return false; }
        if (f.favouritesOnly && !e.favourite) { return false; }
        if (f.extApiOnly && e.kind == RemoteKind::KiwiSDR && !e.hasFlag("ext-api")) { return false; }
        if (m_search[0]) {
            const std::string needle = lower(m_search);
            const std::string hay = lower(e.name + " " + e.location + " " + e.target.host + " " + kindName(e.kind));
            if (hay.find(needle) == std::string::npos) { return false; }
        }
        return true;
    }

    int RemoteBrowser::groupLead(const Group& g) const {
        for (int m : g.members) {
            if (entryVisible(m_entries[m])) { return m; }
        }
        return -1;
    }

    const RemoteEntry* RemoteBrowser::selectedEntry() const {
        for (const auto& e : m_entries) {
            if (e.id == m_selectedEntryId) { return entryVisible(e) ? &e : nullptr; }
        }
        return nullptr;
    }

    void RemoteBrowser::selectGroup(int gi) {
        if (gi < 0 || gi >= (int)m_groups.size()) { return; }
        const int lead = groupLead(m_groups[gi]);
        if (lead < 0) { return; }
        m_selectedGroup = m_groups[gi].key;
        m_selectedEntryId = m_entries[lead].id;
        Group g = m_groups[gi];
        m_groups.erase(m_groups.begin() + gi);
        m_groups.push_back(std::move(g));
        m_groupByKey.clear();
        for (int i = 0; i < (int)m_groups.size(); i++) { m_groupByKey[m_groups[i].key] = i; }
    }

    void RemoteBrowser::selectEntry(int ei) {
        if (ei < 0 || ei >= (int)m_entries.size()) { return; }
        m_selectedGroup = m_entries[ei].target.hostKey();
        m_selectedEntryId = m_entries[ei].id;
        auto it = m_groupByKey.find(m_selectedGroup);
        if (it != m_groupByKey.end()) {
            Group g = m_groups[it->second];
            m_groups.erase(m_groups.begin() + it->second);
            m_groups.push_back(std::move(g));
            m_groupByKey.clear();
            for (int i = 0; i < (int)m_groups.size(); i++) { m_groupByKey[m_groups[i].key] = i; }
        }
    }

    std::optional<std::pair<double, double>> RemoteBrowser::qthLatLon() const {
        return maidenheadToLatLon(m_qth);
    }

    // =========================================================================
    // Popup

    void RemoteBrowser::draw() {
        const std::string popupTitle = m_title + "##remote_browser_" + m_prefix;
        if (m_pendingOpen) {
            m_pendingOpen = false;
            DirectoryService::instance().ensureStarted();
            ImGui::OpenPopup(popupTitle.c_str());
        }
        if (!ImGui::IsPopupOpen(popupTitle.c_str())) { return; }

        auto& dir = DirectoryService::instance();
        if (dir.generation() != m_dirGeneration || recentGeneration() != m_recentGeneration) {
            m_dirGeneration = dir.generation();
            m_recentGeneration = recentGeneration();
            rebuild();
        }
        // Filters, search, handlers and the frequency can all change between
        // frames; one pass here serves every check below.
        updateVisibility();

        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
#ifdef __ANDROID__
        const bool desktopPopup = false;
        const bool fullscreenPopup = shouldUseFullscreenPopup(displaySize);
#else
        const bool desktopPopup = true;
        const bool fullscreenPopup = false;
#endif
        const bool locked = fullscreenPopup || (desktopPopup && m_maximized);
        const ImVec2 popupPos = locked ? ImVec2(0.0f, 0.0f) : displaySize * 0.125f;
        const ImVec2 popupSize = locked ? displaySize : displaySize * 0.75f;
        ImGuiWindowFlags flags = 0;
        if (!desktopPopup) { flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings; }
        if (locked) { flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove; }
        if (fullscreenPopup) { flags |= ImGuiWindowFlags_NoTitleBar; }
        if (desktopPopup && !m_maximized) { ImGui::SetNextWindowSizeConstraints(desktopPopupMinSize(displaySize), displaySize); }
        const ImGuiCond cond = locked ? ImGuiCond_Always : (desktopPopup ? ImGuiCond_FirstUseEver : ImGuiCond_Always);
        ImGui::SetNextWindowPos(popupPos, cond);
        ImGui::SetNextWindowSize(popupSize, cond);
        if (!ImGui::BeginPopupModal(popupTitle.c_str(), nullptr, flags)) { return; }

        drawTopRow();

        const float footerHeight = 2.0f * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y);
        const float bodyHeight = ImMax(1.0f, ImGui::GetContentRegionAvail().y - footerHeight);
        if (m_listView) { drawList(bodyHeight); }
        else { drawMap(bodyHeight, fullscreenPopup); }

        drawFooter(desktopPopup, fullscreenPopup);
        ImGui::EndPopup();
    }

    void RemoteBrowser::drawTopRow() {
        if (ImGui::RadioButton("Map", !m_listView)) {
            m_listView = false;
            saveFilters();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("List", m_listView)) {
            m_listView = true;
            saveFilters();
        }
        ImGui::SameLine();
        if (ImGui::Button("Filters...")) { ImGui::OpenPopup("##remote_filters"); }
        drawFiltersPopup();
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) { DirectoryService::instance().refresh(true); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImMax(ImGui::GetFontSize() * 6.0f, ImGui::GetContentRegionAvail().x * 0.35f));
        if (ImGui::InputTextWithHint("##remote_search", "Search", m_search, sizeof m_search)) { m_rowsDirty = true; }

        // Status: visible count and per-provider state.
        int visible = 0;
        for (const auto& e : m_entries) {
            if (entryVisible(e)) { visible++; }
        }
        std::string status = std::to_string(visible) + " of " + std::to_string(m_entries.size()) + " receivers";
        if (m_onlyKind) { status += std::string(" (") + kindName(*m_onlyKind) + ")"; }
        ImGui::SameLine();
        ImGui::TextUnformatted(status.c_str());
        std::string errors;
        bool loading = false;
        for (const auto& st : DirectoryService::instance().status()) {
            loading |= st.loading;
            if (!st.error.empty()) {
                if (!errors.empty()) { errors += "; "; }
                errors += st.name + ": " + st.error + (st.count ? " (showing the cached list)" : "");
            }
        }
        if (loading) {
            ImGui::SameLine();
            ImGui::TextDisabled("loading...");
        }
        if (!errors.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", errors.c_str());
            ImGui::PopStyleColor();
        }
    }

    void RemoteBrowser::drawFiltersPopup() {
        if (!ImGui::BeginPopup("##remote_filters")) { return; }
        bool changed = false;
        if (m_onlyKind) {
            ImGui::Text("Showing %s only", kindName(*m_onlyKind));
            ImGui::SameLine();
            if (ImGui::SmallButton("Show all kinds")) {
                m_onlyKind.reset();
                changed = true;
            }
        }
        else {
            ImGui::TextUnformatted("Kinds:");
            for (RemoteKind k : kAllKinds) {
                bool shown = m_filters.hiddenKinds.count(k) == 0;
                if (ImGui::Checkbox(kindName(k), &shown)) {
                    if (shown) { m_filters.hiddenKinds.erase(k); }
                    else { m_filters.hiddenKinds.insert(k); }
                    changed = true;
                }
            }
        }
        ImGui::Separator();
        char covers[96];
        std::snprintf(covers, sizeof covers, "Covers the current frequency (%.3f MHz)", (double)gui::freqSelect.frequency / 1e6);
        changed |= ImGui::Checkbox(covers, &m_filters.coversFrequency);
        changed |= ImGui::Checkbox("Free slots only", &m_filters.freeOnly);
        changed |= ImGui::Checkbox("Hide full receivers", &m_filters.hideFull);
        changed |= ImGui::Checkbox("Connectable only (source loaded, TLS supported, IQ)", &m_filters.hideUnconnectable);
        changed |= ImGui::Checkbox("SpyServer: owner allows full control", &m_filters.fullControlOnly);
        changed |= ImGui::Checkbox("OpenWebRX+: IQ capable (1.2.129+)", &m_filters.iqOnly);
        changed |= ImGui::Checkbox("Server waterfall available", &m_filters.waterfallOnly);
        changed |= ImGui::Checkbox("KiwiSDR: EXT API only", &m_filters.extApiOnly);
        changed |= ImGui::Checkbox("Favourites only", &m_filters.favouritesOnly);
        ImGui::Separator();
        ImGui::TextUnformatted("My locator (for distances):");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::InputText("##remote_qth", m_qth, sizeof m_qth)) {
            core::configManager.edit().set(kQthKey, std::string(m_qth));
            m_rowsDirty = true;
        }
        if (m_qth[0] && !qthLatLon()) {
            ImGui::SameLine();
            ImGui::TextDisabled("not a Maidenhead locator");
        }
        if (changed) { saveFilters(); }
        ImGui::EndPopup();
    }

    // =========================================================================
    // Map view

    ImU32 RemoteBrowser::stateColor(const RemoteEntry& e) const {
        if (!sigpath::remoteRegistry.unavailableReason(e).empty()) {
            if (e.tlsRequired && sigpath::remoteRegistry.hasHandler(e.kind)) { return IM_COL32(70, 110, 200, 255); } // needs TLS
            return IM_COL32(90, 90, 90, 255); // not connectable
        }
        if (e.isFull()) { return IM_COL32(205, 0, 0, 255); }
        if (auto occ = e.occupancy(); occ && *occ >= 0.75f) { return IM_COL32(235, 170, 0, 255); }
        return IM_COL32(0, 215, 0, 255);
    }

    void RemoteBrowser::drawMarkerShape(ImDrawList* dl, RemoteKind kind, ImVec2 c, float r, ImU32 fill, ImU32 outline, float thickness) const {
        switch (kind) {
        case RemoteKind::KiwiSDR:
            dl->AddCircleFilled(c, r, fill, 12);
            dl->AddCircle(c, r, outline, 12, thickness);
            break;
        case RemoteKind::UberSDR: {
            const ImVec2 p[4] = { c + ImVec2(0, -r * 1.2f), c + ImVec2(r * 1.2f, 0), c + ImVec2(0, r * 1.2f), c + ImVec2(-r * 1.2f, 0) };
            dl->AddQuadFilled(p[0], p[1], p[2], p[3], fill);
            dl->AddQuad(p[0], p[1], p[2], p[3], outline, thickness);
            break;
        }
        case RemoteKind::SpyServer: {
            const ImVec2 a = c + ImVec2(0, -r * 1.2f), b = c + ImVec2(r * 1.1f, r * 0.9f), d = c + ImVec2(-r * 1.1f, r * 0.9f);
            dl->AddTriangleFilled(a, b, d, fill);
            dl->AddTriangle(a, b, d, outline, thickness);
            break;
        }
        case RemoteKind::OpenWebRX:
            dl->AddRectFilled(c - ImVec2(r, r) * 0.9f, c + ImVec2(r, r) * 0.9f, fill);
            dl->AddRect(c - ImVec2(r, r) * 0.9f, c + ImVec2(r, r) * 0.9f, outline, 0.0f, 0, thickness);
            break;
        default:
            dl->AddNgonFilled(c, r, fill, 6);
            dl->AddNgon(c, r, outline, 6, thickness);
            break;
        }
    }

    std::vector<int> RemoteBrowser::groupsAtScreenPos(ImVec2 screenPos) const {
        std::vector<int> hits;
        if (!m_geoMap.recentMapToScreen) { return hits; }
        const float sz = style::baseFont->FontSize;
#ifdef __ANDROID__
        const float half = sz;
#else
        const float half = sz / 2.0f;
#endif
        const ImVec2 rel = screenPos - m_geoMap.recentCanvasPos;
        // Topmost (last drawn) first.
        for (int i = (int)m_groups.size() - 1; i >= 0; i--) {
            const auto& g = m_groups[i];
            if (!g.cart || groupLead(g) < 0) { continue; }
            const ImVec2 dest = m_geoMap.recentMapToScreen(*g.cart);
            if (std::fabs(dest.x - rel.x) <= half && std::fabs(dest.y - rel.y) <= half) { hits.push_back(i); }
        }
        return hits;
    }

    void RemoteBrowser::drawMarkers() {
        const float sz = style::baseFont->FontSize;
        const float r = sz / 2.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
#ifndef __ANDROID__
        // Desktop hover tooltip; a touch user gets the details from the
        // selection panel.
        const bool windowHovered = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
        const ImVec2 mousePos = ImGui::GetMousePos();
        int hovered = -1;
#endif
        for (int gi = 0; gi < (int)m_groups.size(); gi++) {
            const auto& g = m_groups[gi];
            if (!g.cart) { continue; }
            const int lead = groupLead(g);
            if (lead < 0) { continue; }
            const RemoteEntry& e = m_entries[lead];
            const ImVec2 c = m_geoMap.recentCanvasPos + m_geoMap.recentMapToScreen(*g.cart);
            const bool selected = g.key == m_selectedGroup;
            const ImU32 outline = selected ? IM_COL32(255, 255, 0, 255) : (e.favourite ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255));
            drawMarkerShape(dl, e.kind, c, r, stateColor(e), outline, selected ? 2.5f : 1.0f);
            // A second protocol on the same host: a small dot.
            if (g.members.size() > 1) {
                int visibleMembers = 0;
                for (int m : g.members) { visibleMembers += entryVisible(m_entries[m]) ? 1 : 0; }
                if (visibleMembers > 1) { dl->AddCircleFilled(c + ImVec2(r, -r), r * 0.35f, IM_COL32(255, 255, 255, 255), 8); }
            }
#ifndef __ANDROID__
            if (windowHovered && std::fabs(mousePos.x - c.x) <= r && std::fabs(mousePos.y - c.y) <= r) { hovered = gi; }
#endif
        }
#ifndef __ANDROID__
        if (hovered >= 0) {
            std::ostringstream ss;
            bool first = true;
            for (int m : m_groups[hovered].members) {
                const RemoteEntry& e = m_entries[m];
                if (!entryVisible(e)) { continue; }
                if (!first) { ss << "\n"; }
                first = false;
                ss << kindName(e.kind) << ": " << e.name;
                if (!e.location.empty()) { ss << "\n  " << e.location; }
                if (!e.ranges.empty()) { ss << "\n  " << formatRanges(e); }
                if (e.users && e.usersMax) { ss << "\n  users " << *e.users << "/" << *e.usersMax; }
            }
            ImGui::SetTooltip("%s", ss.str().c_str());
        }
#endif
    }

    void RemoteBrowser::handleMapClick() {
        if (!(ImGui::IsMouseClicked(0) && ImGui::GetMouseClickedCount(0) == 1)) { return; }
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsAnyItemHovered()) { return; }
        const auto hits = groupsAtScreenPos(ImGui::GetMousePos());
        if (hits.empty()) { return; }
        if (hits.size() == 1) {
            selectGroup(hits.front());
            return;
        }
        // Overlapping markers: let the user pick instead of guessing.
        m_clusterPicks.clear();
        for (int gi : hits) {
            const int lead = groupLead(m_groups[gi]);
            if (lead < 0) { continue; }
            const RemoteEntry& e = m_entries[lead];
            m_clusterPicks.push_back({ e.name + " (" + kindName(e.kind) + ")", m_groups[gi].key });
        }
        ImGui::OpenPopup("##remote-cluster-pick");
    }

    void RemoteBrowser::drawClusterPicker() {
        if (!ImGui::BeginPopup("##remote-cluster-pick", ImGuiWindowFlags_NoMove)) { return; }
        ImGui::TextUnformatted("Several receivers here - pick one:");
        const float rowH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
        const bool scroll = m_clusterPicks.size() > 8;
        if (scroll) { ImGui::BeginChild("##cluster-scroll", ImVec2(0.0f, rowH * 8.0f), false); }
        for (size_t k = 0; k < m_clusterPicks.size(); k++) {
            const auto& c = m_clusterPicks[k];
            if (ImGui::Button((c.label + "##pick" + std::to_string(k)).c_str())) {
                auto it = m_groupByKey.find(c.groupKey);
                if (it != m_groupByKey.end()) { selectGroup(it->second); }
                ImGui::CloseCurrentPopup();
            }
        }
        if (scroll) { ImGui::EndChild(); }
        ImGui::EndPopup();
    }

    void RemoteBrowser::drawMap(float height, bool fullscreenPopup) {
        (void)fullscreenPopup;
        // The map never scrolls: panning is the map's own drag.
        ImGui::BeginChild("##remote-geomap", ImVec2(ImGui::GetContentRegionAvail().x, height), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        pushOverlayButtonStyle();
        const char* fullLabel = m_filters.hideFull ? "Show full receivers" : "Hide full receivers";
        m_geoMap.draw(
            fullLabel,
            [this]() {
                m_filters.hideFull = !m_filters.hideFull;
                saveFilters();
            },
            [this]() { drawMarkers(); },
            [this]() -> bool { return !groupsAtScreenPos(ImGui::GetMousePos()).empty(); });
        popOverlayButtonStyle();
        if (m_geoMap.scaleTranslateDirty) {
            m_geoMap.saveTo(*m_config, m_prefix.c_str());
            m_geoMap.scaleTranslateDirty = false;
        }

        handleMapClick();
        drawClusterPicker();
        drawPanel(true);
        ImGui::EndChild();
    }

    // =========================================================================
    // List view

    void RemoteBrowser::drawList(float height) {
        const auto qth = qthLatLon();
        const int64_t now = nowUnix();
        const float panelHeight = selectedEntry() ? ImMin(height * 0.4f, ImGui::GetTextLineHeightWithSpacing() * 9.0f) : 0.0f;
        const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
                                      ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##remote-list", COL_COUNT, flags, ImVec2(0.0f, ImMax(1.0f, height - panelHeight)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 0.0f, COL_NAME);
            ImGui::TableSetupColumn("Kind", 0, 0.0f, COL_KIND);
            ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 0.0f, COL_LOCATION);
            ImGui::TableSetupColumn("km", ImGuiTableColumnFlags_PreferSortAscending, 0.0f, COL_DISTANCE);
            ImGui::TableSetupColumn("Users", 0, 0.0f, COL_USERS);
            ImGui::TableSetupColumn("Range", 0, 0.0f, COL_RANGE);
            ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, COL_SNR);
            ImGui::TableSetupColumn("Seen", 0, 0.0f, COL_SEEN);
            ImGui::TableHeadersRow();

            auto distance = [&](const RemoteEntry& e) -> double {
                if (!qth || !e.latLon) { return 1e9; }
                return distanceKm(*qth, *e.latLon);
            };

            ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
            if (specs && specs->SpecsDirty) {
                m_rowsDirty = true;
                specs->SpecsDirty = false;
            }
            if (m_rowsDirty) {
                m_rowsDirty = false;
                m_rows.clear();
                for (int i = 0; i < (int)m_entries.size(); i++) {
                    if (entryVisible(m_entries[i])) { m_rows.push_back(i); }
                }
                if (specs && specs->SpecsCount > 0) {
                    const auto& s = specs->Specs[0];
                    const bool asc = s.SortDirection == ImGuiSortDirection_Ascending;
                    std::stable_sort(m_rows.begin(), m_rows.end(), [&](int ia, int ib) {
                        const RemoteEntry& a = m_entries[ia];
                        const RemoteEntry& b = m_entries[ib];
                        int c = 0;
                        auto cmp = [](auto x, auto y) { return x < y ? -1 : (y < x ? 1 : 0); };
                        switch (s.ColumnUserID) {
                        case COL_NAME: c = cmp(lower(a.name), lower(b.name)); break;
                        case COL_KIND: c = cmp(kindRank(a.kind), kindRank(b.kind)); break;
                        case COL_LOCATION: c = cmp(lower(a.location), lower(b.location)); break;
                        case COL_DISTANCE: c = cmp(distance(a), distance(b)); break;
                        case COL_USERS: c = cmp(a.occupancy().value_or(2.0f), b.occupancy().value_or(2.0f)); break;
                        case COL_RANGE: {
                            const uint64_t ra = a.ranges.empty() ? 0 : a.ranges.back().second;
                            const uint64_t rb = b.ranges.empty() ? 0 : b.ranges.back().second;
                            c = cmp(ra, rb);
                            break;
                        }
                        case COL_SNR: c = cmp(a.snrDb.value_or(-1000.0f), b.snrDb.value_or(-1000.0f)); break;
                        case COL_SEEN: c = cmp(a.lastSeenUnix.value_or(0), b.lastSeenUnix.value_or(0)); break;
                        default: break;
                        }
                        return asc ? c < 0 : c > 0;
                    });
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin((int)m_rows.size());
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                    const int ei = m_rows[row];
                    const RemoteEntry& e = m_entries[ei];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(COL_NAME);
                    const bool unavailable = !sigpath::remoteRegistry.unavailableReason(e).empty();
                    if (unavailable) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); }
                    const std::string label = (e.favourite ? "* " : "") + e.name + "##row" + e.id;
                    if (ImGui::Selectable(label.c_str(), e.id == m_selectedEntryId,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectEntry(ei);
                        if (ImGui::IsMouseDoubleClicked(0) && !unavailable) { doPick(e); }
                    }
                    ImGui::TableSetColumnIndex(COL_KIND);
                    ImGui::TextUnformatted(kindName(e.kind));
                    ImGui::TableSetColumnIndex(COL_LOCATION);
                    ImGui::TextUnformatted(e.location.c_str());
                    ImGui::TableSetColumnIndex(COL_DISTANCE);
                    if (qth && e.latLon) { ImGui::Text("%.0f", distanceKm(*qth, *e.latLon)); }
                    ImGui::TableSetColumnIndex(COL_USERS);
                    if (e.users && e.usersMax) { ImGui::Text("%d/%d", *e.users, *e.usersMax); }
                    ImGui::TableSetColumnIndex(COL_RANGE);
                    if (!e.ranges.empty()) { ImGui::TextUnformatted(formatRanges(e).c_str()); }
                    ImGui::TableSetColumnIndex(COL_SNR);
                    if (e.snrDb) { ImGui::Text("%.0f", *e.snrDb); }
                    ImGui::TableSetColumnIndex(COL_SEEN);
                    if (e.lastSeenUnix) { ImGui::TextUnformatted(formatAge(now - *e.lastSeenUnix).c_str()); }
                    if (unavailable) { ImGui::PopStyleColor(); }
                }
            }
            ImGui::EndTable();
        }
        if (panelHeight > 0.0f) {
            ImGui::BeginChild("##remote-list-panel", ImVec2(0.0f, panelHeight), false);
            drawPanel(false);
            ImGui::EndChild();
        }
    }

    // =========================================================================
    // Panel and footer

    void RemoteBrowser::drawPanel(bool overlay) {
        const RemoteEntry* e = selectedEntry();
        if (!e) { return; }
        auto line = [overlay](const std::string& s) {
            if (overlay) { doOverlayText("%s", s.c_str()); }
            else { ImGui::TextUnformatted(s.c_str()); }
        };

        // Several protocols on this host: choose which one Connect uses.
        auto git = m_groupByKey.find(m_selectedGroup);
        if (git != m_groupByKey.end()) {
            const auto& g = m_groups[git->second];
            int visibleMembers = 0;
            for (int m : g.members) { visibleMembers += entryVisible(m_entries[m]) ? 1 : 0; }
            if (visibleMembers > 1) {
                for (int m : g.members) {
                    const RemoteEntry& o = m_entries[m];
                    if (!entryVisible(o)) { continue; }
                    if (ImGui::RadioButton((std::string(kindName(o.kind)) + "##proto" + o.id).c_str(), o.id == m_selectedEntryId)) {
                        m_selectedEntryId = o.id;
                    }
                    ImGui::SameLine();
                }
                ImGui::NewLine();
                e = selectedEntry();
                if (!e) { return; }
            }
        }

        line(std::string(kindName(e->kind)) + ": " + e->name);
        if (!e->location.empty()) { line(e->location); }
        for (const auto& [k, v] : e->details) { line(k + ": " + v); }
        if (const auto qth = qthLatLon(); qth && e->latLon) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "Distance: %.0f km", distanceKm(*qth, *e->latLon));
            line(buf);
        }
        line("Listed by: " + e->provider);
        const std::string reason = sigpath::remoteRegistry.unavailableReason(*e);
        if (!reason.empty()) { line("Cannot connect: " + reason); }

        const ProbeState ps = sigpath::remoteRegistry.probeState();
        if (ps.entryId == e->id && ps.result && !ps.running) {
            for (const auto& [k, v] : ps.result->negotiated) { line("Test: " + k + ": " + v); }
        }
    }

    void RemoteBrowser::doPick(const RemoteEntry& e) {
        if (m_pickAction) {
            m_pickAction(e);
        }
        else if (!connectEntry(e)) {
            return;
        }
        ImGui::CloseCurrentPopup();
    }

    void RemoteBrowser::drawFooter(bool desktopPopup, bool fullscreenPopup) {
        (void)fullscreenPopup;
        const ProbeState ps = sigpath::remoteRegistry.probeState();
        if (!ps.status.empty()) { ImGui::TextUnformatted(ps.status.c_str()); }
        else { ImGui::TextDisabled("Select a receiver on the map or in the list."); }

        if (desktopPopup) {
            if (ImGui::Button(m_maximized ? "Restore" : "Maximize")) {
                const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
                if (m_maximized) {
                    m_maximized = false;
                    if (m_restoreValid) {
                        ImGui::SetWindowPos(m_restorePos, ImGuiCond_Always);
                        ImGui::SetWindowSize(m_restoreSize, ImGuiCond_Always);
                    }
                }
                else {
                    m_restorePos = ImGui::GetWindowPos();
                    m_restoreSize = ImGui::GetWindowSize();
                    m_restoreValid = true;
                    m_maximized = true;
                    ImGui::SetWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                    ImGui::SetWindowSize(displaySize, ImGuiCond_Always);
                }
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Close")) {
            sigpath::remoteRegistry.cancelProbe();
            ImGui::CloseCurrentPopup();
        }

        const RemoteEntry* sel = selectedEntry();
        if (!sel) { return; }
        const RemoteEntry e = *sel; // the actions below may rebuild the list
        const std::string reason = sigpath::remoteRegistry.unavailableReason(e);
        const auto handler = sigpath::remoteRegistry.handler(e.kind);

        ImGui::SameLine();
        if (ps.running) {
            if (ImGui::Button("Cancel test")) { sigpath::remoteRegistry.cancelProbe(); }
        }
        else {
            ImGui::BeginDisabled(!reason.empty() || !handler || !handler->probe);
            if (ImGui::Button("Test")) { sigpath::remoteRegistry.startProbe(e); }
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        // websdr_view adds KiwiSDR receivers itself and needs no handler.
        const bool pickable = m_pickAction ? (e.kind == RemoteKind::KiwiSDR && !e.tlsRequired) : reason.empty();
        ImGui::BeginDisabled(!pickable);
        const bool pick = ImGui::Button((m_pickLabel + "##remote_pick").c_str());
        ImGui::EndDisabled();

        ImGui::SameLine();
        const bool fav = e.favourite;
        const bool toggleFav = ImGui::Button(fav ? "Unfavourite" : "Favourite");
        if (e.provider == "recent" && !fav) {
            ImGui::SameLine();
            if (ImGui::Button("Forget")) { forgetRecent(e); }
        }

        if (toggleFav) { setFavourite(e, !fav); }
        if (pick) { doPick(e); }
    }

    // =========================================================================
    // Shared browser and connect flow

    namespace {
        std::unique_ptr<RemoteBrowser> g_browser;

        RemoteBrowser& sharedBrowser() {
            if (!g_browser) {
                g_browser = std::make_unique<RemoteBrowser>(&core::configManager, "remoteBrowser", "Remote receivers");
            }
            return *g_browser;
        }
    }

    void openBrowser(std::optional<RemoteKind> onlyKind) {
        sharedBrowser().open(onlyKind);
    }

    void drawBrowser() {
        if (!g_browser) { return; }
        g_browser->draw();
    }

    bool connectEntry(const RemoteEntry& entry) {
        const auto handler = sigpath::remoteRegistry.handler(entry.kind);
        if (!handler || !handler->connect) { return false; }
        if (!sigpath::remoteRegistry.unavailableReason(entry).empty()) { return false; }
        sigpath::remoteRegistry.cancelProbe();
        if (gui::mainWindow.isPlaying()) { gui::mainWindow.setPlayState(false); }
        if (sourcemenu::selectedSourceName() != handler->sourceName) {
            sourcemenu::selectSource(handler->sourceName);
            core::configManager.edit().set("source", handler->sourceName);
        }
        flog::info("Remote browser: connecting {} to {}", handler->sourceName, entry.target.url());
        handler->connect(entry);
        recordRecent(entry);
        return true;
    }

    void shutdown() {
        sigpath::remoteRegistry.shutdown();
        DirectoryService::shutdown();
    }

}
