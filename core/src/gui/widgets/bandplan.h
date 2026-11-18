#pragma once
#include <gui/widgets/band_mapping.h>
#include <json.hpp>
#include <module.h>
#include <imgui/imgui.h>
#include <stdint.h>

using nlohmann::json;

namespace bandplan {
    struct ResolvedBandData_t {
        // BandMapping registries have static application lifetime. A null
        // mapping denotes a classified legacy row without stable identity.
        const freq_input::BandMapping* mapping = nullptr;
        freq_input::LegacyBandClassification legacy;

        std::string_view bandId() const {
            return mapping ? mapping->bandId : std::string_view{};
        }

        freq_input::BandService service() const {
            return mapping ? mapping->service : legacy.service;
        }

        freq_input::BandFamily family() const {
            return mapping ? mapping->family : legacy.family;
        }

        bool isBandOrSegment() const noexcept {
            return legacy.isBandOrSegment();
        }
    };

    struct Band_t {
        std::string name;
        std::string type;
        double start;
        double end;
        // Static stable identity plus the runtime classification of this
        // particular legacy row. Different services may overlap.
        ResolvedBandData_t resolved;
        // Optional tuning defaults (0 / empty = absent), sparse KiwiSDR-derived
        // enrichment; see scripts/enrich_bandplans.py.
        double defFreq = 0;
        std::string defMode;
        double chan = 0;

        bool hasValidFrequencySpan() const noexcept;
        bool containsFrequency(double frequency) const noexcept;
    };

    void from_json(const json& j, Band_t& b);

    struct BandPlan_t {
        std::string name;
        std::string countryName;
        std::string countryCode;
        std::string authorName;
        std::string authorURL;
        std::vector<Band_t> bands;
        // Runtime content identity for caches holding pointers into `bands`.
        // A newly loaded plan receives a new revision even if its map address,
        // name, row count, and vector allocation happen to be reused.
        uint64_t revision = 0;

        // A stable band may be represented by adjacent or disjoint source
        // segments. Return the first eligible segment containing the
        // frequency, preserving source order.
        const Band_t* findMappedSegmentAtFrequency(
            const freq_input::BandMapping& mapping,
            double frequency) const;
    };

    void from_json(const json& j, BandPlan_t& b);

    struct BandPlanColor_t {
        // Defaults match the waterfall's fallback for an untyped band, so a
        // default-constructed entry is never an indeterminate color.
        uint32_t colorValue = IM_COL32(255, 255, 255, 255);
        uint32_t transColorValue = IM_COL32(255, 255, 255, 100);
    };

    void from_json(const json& j, BandPlanColor_t& ct);

    void loadBandPlan(const std::string& path);
    void loadFromDir(const std::string& path);
    // Replaces the color table. Malformed entries are reported and skipped;
    // the call does not propagate parse errors out of the user's config.
    void loadColorTable(const json& table);

    // Exported so tests and modules use the library's single copy.
    SDRPP_EXPORT std::map<std::string, BandPlan_t> bandplans;
    SDRPP_EXPORT std::vector<std::string> bandplanNames;
    SDRPP_EXPORT std::string bandplanNameTxt;
    SDRPP_EXPORT std::map<std::string, BandPlanColor_t> colorTable;
};
