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
#ifdef _WIN32
#define _WINSOCKAPI_ // stops windows.h including winsock.h
#endif

#include <json.hpp>
#include <imgui/imgui.h>
#include <stdint.h>
#include "config.h"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace geomap {

    constexpr double pi = 3.14159265358979323846;
    constexpr double earthRadius = 6371.0; // Earth radius in kilometers

    inline double degToRad(double deg) {
        return deg * pi / 180.0;
    }
    inline double radToDeg(double rad) {
        return rad * 180.0 / pi;
    }

    struct GeoCoordinates {
        double latitude;
        double longitude;

        std::string toString() const {
            char buf[128];
            std::snprintf(buf, sizeof buf, "Lat: %.6f, Lon: %.6f", latitude, longitude);
            return std::string(buf);
        }
    };

    struct CartesianCoordinates {
        double x;
        double y;

        ImVec2 toImVec2() const {
            return ImVec2(static_cast<float>(x), static_cast<float>(y));
        }
    };

    // One polygon (outer ring + optional hole rings) with its earcut
    // triangulation. `border` is the flat vertex list (outer ring first,
    // then each hole ring concatenated); `ringStarts` records where each
    // ring begins, with a final sentinel equal to `border.size()` so a
    // ring spans `[ringStarts[i], ringStarts[i+1])`. `triangles` indexes
    // into `border` and excludes hole regions (earcut handles that when
    // given multiple rings). For the day/night terminator and other
    // simple shapes there are no holes and `ringStarts == {0, N}`.
    // `triangles` is empty for degenerate polygons (< 3 outer vertices
    // or earcut failure); in that case only the outline is rendered.
    struct Polygon {
        std::vector<CartesianCoordinates> border;
        std::vector<size_t> ringStarts;
        std::vector<uint32_t> triangles;
        // Axis-aligned bbox of the OUTER ring (holes lie inside it), in
        // cartesian map space. Cheap reject before point-in-polygon hover
        // tests. Inverted (min > max) for empty polygons.
        CartesianCoordinates bbMin{ 1.0, 1.0 };
        CartesianCoordinates bbMax{ -1.0, -1.0 };
    };


    inline CartesianCoordinates geoToCartesian(const GeoCoordinates& geo) {
        double latRad = degToRad(geo.latitude);
        double lngRad = degToRad(geo.longitude);

        double x = lngRad / pi;
        double y = latRad / (pi / 2.0);

        return { x, y };
    }

    inline GeoCoordinates cartesianToGeo(const CartesianCoordinates& cart) {
        return { radToDeg(cart.y * (pi / 2.0)), radToDeg(cart.x * pi) };
    }

    // Maidenhead 6-character grid square (e.g. "JN78dq") for the given
    // coordinates. Used by ham operators as a compact location identifier.
    // Returns the empty string if the coordinates are out of range.
    inline std::string geoToMaidenhead(const GeoCoordinates& geo) {
        if (geo.latitude < -90.0 || geo.latitude > 90.0 ||
            geo.longitude < -180.0 || geo.longitude > 180.0) {
            return {};
        }
        double lon = geo.longitude + 180.0;  // 0..360
        double lat = geo.latitude  + 90.0;   // 0..180

        const int lonField = std::min(17, (int)(lon / 20.0));
        const int latField = std::min(17, (int)(lat / 10.0));
        const int lonSquare = (int)((lon - lonField * 20.0) / 2.0);
        const int latSquare = (int)(lat - latField * 10.0);
        const int lonSub = (int)((lon - lonField * 20.0 - lonSquare * 2.0) * 12.0);
        const int latSub = (int)((lat - latField * 10.0 - latSquare * 1.0) * 24.0);

        char buf[7];
        std::snprintf(buf, sizeof buf, "%c%c%d%d%c%c",
                      'A' + lonField,
                      'A' + latField,
                      lonSquare,
                      latSquare,
                      'a' + lonSub,
                      'a' + latSub);
        return std::string(buf);
    }


    struct GeoMap {

        ImVec2 scale = ImVec2(1.0, 1.0);
        ImVec2 translate = ImVec2(0.0, 0.0); // in map coordinates
        bool scaleTranslateDirty = false;

        std::shared_ptr<ImVec2> initialTouchPos;
        ImVec2 initialTranslate;

        std::function <ImVec2(ImVec2)> recentMapToScreen;
        ImVec2 recentCanvasPos;

        // `overlayDrawer` is invoked AFTER the map content (background,
        // countries, terminator) and input handling, but BEFORE the
        // bottom-row buttons are submitted. Use it to draw map overlays
        // (markers, route lines, etc.) that should sit above the map but
        // below the controls — otherwise raw drawList calls submitted
        // after `draw()` returns would paint on top of the buttons.
        //
        // `overlayHitTest` reports whether the pointer is currently over an
        // interactive overlay object (e.g. a station marker). Those objects
        // are raw drawList primitives that ImGui's item system can't see, so
        // without this hook the country tooltip and the drag-to-pan handler
        // would both fire "through" them. When it returns true we suppress
        // the tooltip and refuse to start a pan, letting the overlay own the
        // click.
        void draw(const char* extraButtonLabel = nullptr,
                  std::function<void()> extraButtonAction = {},
                  std::function<void()> overlayDrawer = {},
                  std::function<bool()> overlayHitTest = {});
        void saveTo(ConfigManager &manager, const char* string);
        void loadFrom(ConfigManager& manager, const char* prefix);
    };

};
