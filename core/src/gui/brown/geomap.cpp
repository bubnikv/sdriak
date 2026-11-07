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
#include <gui/brown/geomap.h>
#include <gui/brown/earcut.hpp>
#include <cmath>
#include <ctime>
#include <fstream>
#include <core.h>
#include <utils/flog.h>
#include <gui/widgets/simple_widgets.h>
#include <filesystem>
#include <utility>
#include <imgui/imgui_internal.h>

// earcut adapter so the triangulator can read x/y from CartesianCoordinates.
namespace mapbox { namespace util {
    template <std::size_t I> struct nth<I, geomap::CartesianCoordinates> {
        static auto get(const geomap::CartesianCoordinates& t) {
            if constexpr (I == 0) return t.x;
            else return t.y;
        }
    };
}}

namespace geomap {


    // Converts degrees to radians

    nlohmann::json geoJSON;
    // [country][polygon] → border + triangles. Triangulated once at load.
    std::vector<std::vector<Polygon>> countriesGeo;
    std::vector<std::string> countryNames;
    // mapcolor13 index per country (Natural Earth's precomputed graph
    // coloring — adjacent countries get different indices). 0 if absent.
    std::vector<int> countryColorIndices;

    // Day/night terminator overlay. Recomputed roughly once per minute from
    // the current UTC time; rendered as a translucent dark fill over the
    // night side of the world.
    Polygon terminatorPolygon;
    std::tm  terminatorPolygonTime{};

    static std::tm getTimeUtc() {
        std::time_t now = std::time(nullptr);
        std::tm utc{};
#if defined(_WIN32) || defined(_WIN64)
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        return utc;
    }

    // Day-of-year (1..366); used by the solar-declination approximation.
    static int dayOfYear(int year, int month, int day) {
        static const int monthDays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
        int doy = 0;
        for (int m = 0; m < month - 1; ++m) doy += monthDays[m];
        doy += day;
        if (month > 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) doy += 1;
        return doy;
    }

    // Solar declination in radians (Spencer 1971 approximation).
    static double solarDeclination(int year, int month, int day) {
        const int N = dayOfYear(year, month, day);
        const double gamma = 2.0 * pi / 365.0 * (N - 1);
        return 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma)
             - 0.006758 * std::cos(2 * gamma) + 0.000907 * std::sin(2 * gamma)
             - 0.002697 * std::cos(3 * gamma) + 0.001480 * std::sin(3 * gamma);
    }

    // Builds the closed polygon describing the night side of the planet for
    // the given UTC time: the terminator curve in longitude steps, closed
    // along whichever pole is in shadow.
    static std::vector<GeoCoordinates> buildNightPolygon(const std::tm& utc) {
        std::vector<GeoCoordinates> polygon;
        const double decl = solarDeclination(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
        const int lonStep = 2;
        const double utcHours = utc.tm_hour + utc.tm_min / 60.0 + utc.tm_sec / 3600.0;

        for (int lon = -180; lon <= 180; lon += lonStep) {
            const double lst = utcHours + lon / 15.0;
            const double H = degToRad((lst - 12.0) * 15.0);
            double lat;
            if (std::fabs(std::cos(decl)) < 1e-6) {
                lat = (decl > 0) ? 90.0 : -90.0;
            }
            else {
                lat = radToDeg(std::atan(-std::cos(H) / std::tan(decl)));
            }
            if (lat > 90.0) lat = 90.0;
            if (lat < -90.0) lat = -90.0;
            polygon.push_back({ lat, double(lon) });
        }
        // Close along the pole that is currently in shadow.
        const bool northInShadow = !(decl > 0);
        const double poleLat = northInShadow ? 90.0 : -90.0;
        polygon.push_back({ poleLat,  180.0 });
        polygon.push_back({ poleLat, -180.0 });
        polygon.push_back(polygon.front());
        return polygon;
    }

    static void updateTerminatorPolygon(const std::tm& utc) {
        const auto geo = buildNightPolygon(utc);
        Polygon poly;
        poly.border.reserve(geo.size());
        for (const auto& g : geo) {
            poly.border.emplace_back(geoToCartesian(g));
        }
        // Single ring, no holes.
        poly.ringStarts = { 0, poly.border.size() };
        if (poly.border.size() >= 3) {
            std::vector<std::vector<CartesianCoordinates>> rings = { poly.border };
            poly.triangles = mapbox::earcut<uint32_t>(rings);
        }
        terminatorPolygon = std::move(poly);
        terminatorPolygonTime = utc;
    }

    static void checkTerminatorPolygon() {
        const std::tm utc = getTimeUtc();
        const std::tm& old = terminatorPolygonTime;
        // Update once per minute — the terminator moves at ~0.25°/min so
        // sub-minute updates would be wasted recomputation.
        const bool sameMinute =
            utc.tm_year == old.tm_year && utc.tm_mon == old.tm_mon && utc.tm_mday == old.tm_mday &&
            utc.tm_hour == old.tm_hour && utc.tm_min == old.tm_min;
        if (sameMinute && !terminatorPolygon.border.empty()) {
            return;
        }
        updateTerminatorPolygon(utc);
    }

    nlohmann::json readGeoJSONFile(const std::string& filePath) {
        std::ifstream fileStream(filePath);

        if (!fileStream.is_open()) {
            flog::error("Failed to open the file {}", filePath);
            return nullptr;
        }

        nlohmann::json geoJSON;
        fileStream >> geoJSON;

        return geoJSON;
    }

    // Span-style ray-casting test so callers can pass any ring (outer or
    // hole) without copying a sub-vector.
    static bool pointInPolygon(const CartesianCoordinates& p, const CartesianCoordinates* verts, size_t count) {
        if (count < 3) return false;
        bool inside = false;
        for (size_t i = 0, j = count - 1; i < count; j = i++) {
            const auto& a = verts[i];
            const auto& b = verts[j];
            const bool crosses = ((a.y > p.y) != (b.y > p.y)) &&
                (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x);
            if (crosses) {
                inside = !inside;
            }
        }
        return inside;
    }

    // True if `p` is strictly inside the polygon's outer ring AND not
    // inside any of its holes — i.e. inside the polygon's actual area.
    static bool pointInPolygonWithHoles(const CartesianCoordinates& p, const Polygon& poly) {
        if (poly.ringStarts.size() < 2) return false;
        const auto* base = poly.border.data();
        const size_t outerStart = poly.ringStarts[0];
        const size_t outerEnd = poly.ringStarts[1];
        if (!pointInPolygon(p, base + outerStart, outerEnd - outerStart)) return false;
        for (size_t r = 1; r + 1 < poly.ringStarts.size(); r++) {
            const size_t hStart = poly.ringStarts[r];
            const size_t hEnd = poly.ringStarts[r + 1];
            if (pointInPolygon(p, base + hStart, hEnd - hStart)) return false;
        }
        return true;
    }

    void maybeInit() {
        if (geoJSON.empty()) {
            std::string resDir = core::getResourcesDirectory();
            const std::string filePath = resDir + "/cty/map.json";
            geoJSON = readGeoJSONFile(filePath);


            for (const auto& feature : geoJSON["features"]) {
                countriesGeo.emplace_back();
                std::string countryName;
                int colorIndex = 0;
                if (feature.contains("properties")) {
                    const auto& props = feature["properties"];
                    if (props.contains("name")) {
                        countryName = props["name"].get<std::string>();
                    }
                    if (props.contains("mapcolor13")) {
                        colorIndex = props["mapcolor13"].get<int>();
                        if (colorIndex < 0) colorIndex = -colorIndex;
                    }
                }
                countryNames.push_back(std::move(countryName));
                countryColorIndices.push_back(colorIndex);

                // Dispatch on GeoJSON geometry type. Polygon coordinates are
                // [outer_ring, hole_ring…]; MultiPolygon is one nesting deeper:
                // [polygon, …] where each polygon is itself [outer_ring,
                // hole_ring…]. Each polygon (with all its rings) becomes one
                // `Polygon` entry — earcut triangulates outer minus holes.
                const auto& geometry = feature["geometry"];
                const std::string type = geometry.contains("type")
                    ? geometry["type"].get<std::string>() : std::string();

                auto addPolygon = [&](const auto& rings) {
                    // Build the rings as a nested vector for earcut, then
                    // flatten into the Polygon's `border` so triangles
                    // (which are indices into the flat list) point at the
                    // right vertices.
                    std::vector<std::vector<CartesianCoordinates>> ringList;
                    for (const auto& ring : rings) {
                        ringList.emplace_back();
                        for (const auto& coord : ring) {
                            const double longitude = coord[0].template get<double>();
                            const double latitude = coord[1].template get<double>();
                            ringList.back().push_back(geoToCartesian({ latitude, longitude }));
                        }
                    }
                    if (ringList.empty()) return;

                    countriesGeo.back().emplace_back();
                    Polygon& dest = countriesGeo.back().back();
                    for (const auto& r : ringList) {
                        dest.ringStarts.push_back(dest.border.size());
                        dest.border.insert(dest.border.end(), r.begin(), r.end());
                    }
                    dest.ringStarts.push_back(dest.border.size());

                    if (ringList[0].size() >= 3) {
                        dest.triangles = mapbox::earcut<uint32_t>(ringList);
                    }

                    // bbox from outer ring; holes lie inside it by definition.
                    const auto& outer = ringList[0];
                    if (!outer.empty()) {
                        dest.bbMin = dest.bbMax = outer.front();
                        for (const auto& p : outer) {
                            if (p.x < dest.bbMin.x) dest.bbMin.x = p.x;
                            if (p.y < dest.bbMin.y) dest.bbMin.y = p.y;
                            if (p.x > dest.bbMax.x) dest.bbMax.x = p.x;
                            if (p.y > dest.bbMax.y) dest.bbMax.y = p.y;
                        }
                    }
                };

                if (type == "Polygon") {
                    addPolygon(geometry["coordinates"]);
                }
                else if (type == "MultiPolygon") {
                    for (const auto& polygon : geometry["coordinates"]) {
                        addPolygon(polygon);
                    }
                }
            }
        }
    }

    // 13-color palette indexed by Natural Earth's `mapcolor13` property,
    // a precomputed graph coloring where no two adjacent countries share
    // a color. Stable across loads — no hue change as data shifts.
    const std::vector<ImVec4> mapcolor13 = {
        ImVec4(0.85f, 0.37f, 0.37f, 1.0f),  // 0  red
        ImVec4(0.37f, 0.85f, 0.37f, 1.0f),  // 1  green
        ImVec4(0.37f, 0.37f, 0.85f, 1.0f),  // 2  blue
        ImVec4(0.85f, 0.85f, 0.37f, 1.0f),  // 3  yellow
        ImVec4(0.85f, 0.37f, 0.85f, 1.0f),  // 4  magenta
        ImVec4(0.37f, 0.85f, 0.85f, 1.0f),  // 5  cyan
        ImVec4(0.85f, 0.62f, 0.37f, 1.0f),  // 6  orange
        ImVec4(0.62f, 0.85f, 0.37f, 1.0f),  // 7  light green
        ImVec4(0.37f, 0.62f, 0.85f, 1.0f),  // 8  light blue
        ImVec4(0.85f, 0.37f, 0.62f, 1.0f),  // 9  pink
        ImVec4(0.62f, 0.37f, 0.85f, 1.0f),  // 10 purple
        ImVec4(0.37f, 0.85f, 0.62f, 1.0f),  // 11 mint
        ImVec4(0.85f, 0.85f, 0.85f, 1.0f)   // 12 light grey
    };


    void GeoMap::draw(const char* extraButtonLabel,
                      std::function<void()> extraButtonAction,
                      std::function<void()> overlayDrawer,
                      std::function<bool()> overlayHitTest) {

        maybeInit();

        const ImVec2 curpos = ImGui::GetCursorPos();


        ImDrawList* drawList = ImGui::GetWindowDrawList();
        recentCanvasPos = ImGui::GetCursorScreenPos();
        auto windowWidth = ImGui::GetContentRegionAvail().x;
        auto windowHeight = ImGui::GetContentRegionAvail().y;
        ImVec2 w2 = ImVec2(windowWidth / 2, windowHeight / 2);
        auto toView = [=](ImVec2 c) {
            c *= scale;
            auto x1 = static_cast<float>(c.x * w2.x + w2.x);
            auto y1 = static_cast<float>(windowHeight - (c.y * w2.y + w2.y));
            return ImVec2(x1, y1) + translate * w2 * scale;
        };
        auto toMap = [=](ImVec2 p) {
            return CartesianCoordinates{
                (p.x - w2.x) / (w2.x * scale.x) - translate.x,
                (w2.y - p.y) / (w2.y * scale.y) + translate.y
            };
        };
        recentMapToScreen = toView;

        // Zoom by `factor` while keeping the map point under `pivotRel`
        // (canvas-relative pixels) fixed on screen. Reads/writes the live
        // scale & translate, so it must recompute the map coord with the new
        // scale rather than go through the value-captured toMap lambda above.
        // Used by both wheel/pinch zoom (pivot = cursor/centroid) and the
        // Zoom buttons (pivot = viewport centre).
        auto zoomAround = [&](ImVec2 pivotRel, float factor) {
            const CartesianCoordinates before = {
                (pivotRel.x - w2.x) / (w2.x * scale.x) - translate.x,
                (w2.y - pivotRel.y) / (w2.y * scale.y) + translate.y
            };
            scale = scale * factor;
            const CartesianCoordinates after = {
                (pivotRel.x - w2.x) / (w2.x * scale.x) - translate.x,
                (w2.y - pivotRel.y) / (w2.y * scale.y) + translate.y
            };
            translate.x += float(after.x - before.x);
            translate.y -= float(after.y - before.y);
            scaleTranslateDirty = true;
        };

        if (windowWidth == 0) {
            return;
        }
        // Country-tooltip gating: cursor must be inside our pane AND not
        // over a registered widget. IsWindowHovered alone would also fire
        // when the cursor is on the Zoom/Reset buttons in this same child
        // window (the button is "in" the window). IsAnyItemHovered alone
        // would also fire from buttons in unrelated panes. Both together
        // is the right condition. Dropping AllowWhenBlockedByActiveItem
        // also makes the tooltip disappear during a map pan (the drag
        // owns ActiveID), which is the right UX.
        const bool mapHovered = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
        const CartesianCoordinates mouseMapPos = toMap(ImGui::GetMousePos() - recentCanvasPos);
        const std::string* hoveredCountry = nullptr;

        // Is the pointer over an interactive overlay object (marker)? Those
        // aren't ImGui items, so the tooltip and pan handlers below can't
        // detect them on their own — the caller's hit-test does. Evaluated
        // once here against this frame's recentMapToScreen (set above).
        const bool pointerOverOverlay = overlayHitTest && overlayHitTest();

        // Dark-blue "water" — also makes the day/night terminator (drawn
        // later as semi-transparent black) actually visible. A black
        // background would blend with the shadow and hide it.
        drawList->AddRectFilled(recentCanvasPos + ImVec2(0, 0), recentCanvasPos + ImGui::GetContentRegionAvail(), ImColor(10, 30, 60));

        // Disable AA fill while triangles are rasterized: each
        // AddTriangleFilled() with AA on emits a fringe along its edges,
        // and at every interior triangulation edge the two adjacent
        // triangles' fringes overlap, double-blending the translucent
        // fill into a visible seam. AA lines stay enabled, so AddLine()
        // outlines below remain smooth.
        const auto savedDrawFlags = drawList->Flags;
        drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;

        for (size_t countryIndex = 0; countryIndex < countriesGeo.size(); countryIndex++) {
            auto& country = countriesGeo[countryIndex];
            const int colorIndex = (countryIndex < countryColorIndices.size())
                ? countryColorIndices[countryIndex] : 0;
            const ImColor lineColor = ImColor(mapcolor13[colorIndex % mapcolor13.size()]);
            // Fill is the same hue as the outline but heavily diluted so the
            // map stays readable under a busy marker layer.
            const ImU32 fillCol = ImGui::ColorConvertFloat4ToU32(
                ImVec4(lineColor.Value.x, lineColor.Value.y, lineColor.Value.z, 0.18f));
            const ImU32 lineCol = ImColor(lineColor);
            for (auto& polygon : country) {
                // Filled triangles first, outline on top.
                for (size_t t = 0; t + 2 < polygon.triangles.size(); t += 3) {
                    const auto& a = polygon.border[polygon.triangles[t]];
                    const auto& b = polygon.border[polygon.triangles[t + 1]];
                    const auto& c = polygon.border[polygon.triangles[t + 2]];
                    drawList->AddTriangleFilled(
                        recentCanvasPos + toView(a.toImVec2()),
                        recentCanvasPos + toView(b.toImVec2()),
                        recentCanvasPos + toView(c.toImVec2()),
                        fillCol);
                }
                if (polygon.ringStarts.size() >= 2) {
                    const size_t outerStart = polygon.ringStarts[0];
                    const size_t outerEnd = polygon.ringStarts[1];

                    // Bbox reject before the O(N) point-in-polygon. With ~200
                    // countries × dozens of polygons each, this is a real
                    // per-frame win.
                    const bool inBBox =
                        mouseMapPos.x >= polygon.bbMin.x && mouseMapPos.x <= polygon.bbMax.x &&
                        mouseMapPos.y >= polygon.bbMin.y && mouseMapPos.y <= polygon.bbMax.y;
                    if (mapHovered && !hoveredCountry && inBBox &&
                        countryIndex < countryNames.size() && !countryNames[countryIndex].empty() &&
                        pointInPolygonWithHoles(mouseMapPos, polygon)) {
                        hoveredCountry = &countryNames[countryIndex];
                    }

                    // Outline the outer ring only. A hole's outline belongs
                    // to whatever country fills that void (Lesotho draws its
                    // own boundary as a separate Polygon feature); drawing
                    // it here too would paint a duplicate outline in the
                    // wrong color.
                    if (outerEnd > outerStart + 1) {
                        for (size_t i = outerStart; i + 1 < outerEnd; i++) {
                            drawList->AddLine(
                                recentCanvasPos + toView(polygon.border[i].toImVec2()),
                                recentCanvasPos + toView(polygon.border[i + 1].toImVec2()),
                                lineCol, 1.0f);
                        }
                    }
                }
            }
        }

        // Day/night terminator: shade the night side. Drawn after countries
        // so it tints the land on the dark side of the planet.
        checkTerminatorPolygon();
        const ImU32 nightFill = ImColor(0, 0, 0, 110);
        for (size_t t = 0; t + 2 < terminatorPolygon.triangles.size(); t += 3) {
            const auto& a = terminatorPolygon.border[terminatorPolygon.triangles[t]];
            const auto& b = terminatorPolygon.border[terminatorPolygon.triangles[t + 1]];
            const auto& c = terminatorPolygon.border[terminatorPolygon.triangles[t + 2]];
            drawList->AddTriangleFilled(
                recentCanvasPos + toView(a.toImVec2()),
                recentCanvasPos + toView(b.toImVec2()),
                recentCanvasPos + toView(c.toImVec2()),
                nightFill);
        }
        drawList->Flags = savedDrawFlags;

        if (hoveredCountry && !pointerOverOverlay) {
            const GeoCoordinates geoPos = cartesianToGeo(mouseMapPos);
            const std::string tooltip = *hoveredCountry + "\n" + geoToMaidenhead(geoPos) + "\n" + geoPos.toString();
            ImGui::SetTooltip("%s", tooltip.c_str());
        }

        // Mouse-wheel zoom anchored to the cursor: compute the map coord
        // under the cursor before and after the scale change and adjust
        // translate so that point stays put. The y delta has the opposite
        // sign of x because our projection inverts the screen y axis.
        if (mapHovered) {
            float wheel = ImGui::GetIO().MouseWheel;
#ifdef __ANDROID__
            // The Android backend translates a two-finger pinch into a
            // horizontal-wheel event and parks the cursor at the pinch
            // centroid, so folding MouseWheelH in here is what makes
            // pinch-to-zoom work — anchored at the centroid via GetMousePos.
            wheel += ImGui::GetIO().MouseWheelH;
#endif
            if (wheel != 0.0f) {
                const ImVec2 mouseRel = ImGui::GetMousePos() - recentCanvasPos;
                // powf so magnitude matters: a desktop notch (wheel = ±1)
                // still gives the old ×1.2, while a continuous pinch delta
                // zooms smoothly instead of snapping.
                zoomAround(mouseRel, std::pow(1.2f, wheel));
            }
        }

        // Drag-to-pan, gated on a synthetic ActiveID. Without this:
        //   - clicks anywhere on screen with the mouse already down would
        //     pan the map (e.g. a click started on a button outside the
        //     popup, then dragged in, would pan);
        //   - the parent popup window could be moved while the user
        //     intended to drag the map.
        // We only initiate when the click *starts* over the map area with
        // no other widget hovered, take the ActiveID for the duration, and
        // release it when the mouse comes up.
        const ImGuiID dragId = ImGui::GetCurrentWindow()->GetID("geomap_drag");
        const bool dragActive = (ImGui::GetActiveID() == dragId);
        // A click that lands on a marker is consumed by the overlay's own
        // click-to-select — it must not also start a map pan, or a tap that
        // drifts would both select and drag. `pointerOverOverlay` gates that.
        if (!dragActive && ImGui::IsMouseClicked(0) && mapHovered && !ImGui::IsAnyItemHovered() && !pointerOverOverlay) {
            ImGui::SetActiveID(dragId, ImGui::GetCurrentWindow());
            ImGui::FocusWindow(ImGui::GetCurrentWindow());
            initialTouchPos = std::make_shared<ImVec2>(ImGui::GetMousePos());
            initialTranslate = translate;
        }
        else if (dragActive) {
            if (ImGui::IsMouseReleased(0)) {
                ImGui::ClearActiveID();
                initialTouchPos.reset();
            }
            else if (initialTouchPos) {
                translate = initialTranslate + (ImGui::GetMousePos() - *initialTouchPos) / w2 / scale;
                scaleTranslateDirty = true;
            }
        }

        // Map overlays (markers, route lines, etc.) draw here — between
        // the map content and the button row, so the buttons end up on
        // top of any overlay rectangles that happen to lie under them.
        if (overlayDrawer) {
            overlayDrawer();
        }

        ImGui::SetCursorPos(curpos);
        // Anchor button zoom to the viewport centre (pivot = w2) so the map
        // grows/shrinks around what the user is looking at, instead of the
        // projection origin off in the Atlantic.
        if (ImGui::Button("Zoom In##geomap-zoom-in")) {
            zoomAround(w2, 2.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Zoom Out##geomap-zoom-out")) {
            zoomAround(w2, 0.5f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Map##reset-map")) {
            scale = ImVec2(1.0, 1.0);
            translate = ImVec2(0.0, 0.0);
            scaleTranslateDirty = true;
        }
        if (extraButtonLabel && extraButtonAction) {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
            if (ImGui::Button(extraButtonLabel)) {
                extraButtonAction();
            }
        }

    }
    void GeoMap::saveTo(ConfigManager& manager, const char* prefix){
        auto pref = std::string(prefix);
        auto configAccess = manager.edit();
        configAccess.set(pref + "_scale_x", scale.x);
        configAccess.set(pref + "_scale_y", scale.y);
        configAccess.set(pref + "_translate_x", translate.x);
        configAccess.set(pref + "_translate_y", translate.y);
    };

    void GeoMap::loadFrom(ConfigManager& manager, const char* prefix) {
        auto pref = std::string(prefix);
        auto configAccess = manager.read();
        if (configAccess.contains(pref + "_scale_x")) {
            configAccess.tryGet(pref + "_scale_x", scale.x);
            configAccess.tryGet(pref + "_scale_y", scale.y);
            configAccess.tryGet(pref + "_translate_x", translate.x);
            configAccess.tryGet(pref + "_translate_y", translate.y);
        }
    };
};
