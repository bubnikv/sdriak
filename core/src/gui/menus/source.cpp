#include <gui/menus/source.h>
#include <imgui.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/main_window.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <utils/optionlist.h>
#include <gui/dialogs/dialog_box.h>
#include <gui/widgets/popup_dialog.h>
#include <gui/remote/browser.h>

namespace sourcemenu {
    int sourceId = 0;
    EventHandler<std::string> sourcesChangedHandler;
    EventHandler<std::string> sourceUnregisterHandler;
    OptionList<std::string, std::string> sources;
    std::string selectedSource;

    int decimId = 0;
    OptionList<int, int> decimations;

    bool iqCorrection = false;
    bool invertIQ = false;

    int offsetId = 0;
    double manualOffset = 0.0;
    std::string selectedOffset;
    double effectiveOffset = 0.0;
    OptionList<std::string, double> offsets;
    std::map<std::string, double> namedOffsets;

    PopupDialog addOffsetDlg;
    char newOffsetName[1024];
    double newOffset = 0.0;

    bool showDelOffsetDialog = false;
    std::string delOffsetName = "";

    // Offset IDs
    enum {
        OFFSET_ID_NONE,
        OFFSET_ID_MANUAL,
        OFFSET_ID_CUSTOM_BASE
    };

    void updateOffset() {
        // Compute the effective offset
        switch (offsetId) {
        case OFFSET_ID_NONE:
            effectiveOffset = 0;
            break;
        case OFFSET_ID_MANUAL:
            effectiveOffset = manualOffset;
            break;
        default:
            effectiveOffset = namedOffsets[offsets.name(offsetId)];
            break;
        }

        // Apply it
        sigpath::sourceManager.setTuningOffset(effectiveOffset);
    }

    void selectOffsetById(int id) {
        // Update the offset mode
        offsetId = id;
        selectedOffset = offsets.name(id);

        // Update the offset
        updateOffset();
    }

    void selectOffsetByName(const std::string& name) {
        // If the name doesn't exist, select 'None'
        if (!offsets.nameExists(name)) {
            selectOffsetById(OFFSET_ID_NONE);
            return;
        }

        // Select using the ID associated with the name
        selectOffsetById(offsets.nameId(name));
    }

    void refreshSources() {
        // Get sources
        auto sourceNames = sigpath::sourceManager.getSourceNames();

        // Define source options
        sources.clear();
        for (auto name : sourceNames) {
            sources.define(name, name, name);
        }
    }

    void selectSource(std::string name) {
        // If there is no source, give up
        if (sources.empty()) {
            sourceId = 0;
            selectedSource.clear();
            return;
        }

        // If a source with the given name doesn't exist, select the first source instead
        if (!sources.valueExists(name)) {
            selectSource(sources.value(0));
            return;
        }

        // Update the GUI variables
        sourceId = sources.valueId(name);
        selectedSource = name;

        // Select the source module
        sigpath::sourceManager.selectSource(name);
    }

    const std::string& selectedSourceName() {
        return selectedSource;
    }

    void onSourcesChanged(std::string name, void* ctx) {
        // Update the source list
        refreshSources();

        // Reselect the current source
        selectSource(selectedSource);
    }

    void onSourceUnregister(std::string name, void* ctx) {
        if (name != selectedSource) { return; }

        gui::mainWindow.setPlayState(false);
    }

    void reloadOffsets() {
        // Clear list
        offsets.clear();
        namedOffsets.clear();

        // Define special offset modes
        offsets.define("None", OFFSET_ID_NONE);
        offsets.define("Manual", OFFSET_ID_MANUAL);

        // Load custom offsets
        {
            auto configAccess = core::configManager.read();
            if (const json* ofs = configAccess.section("offsets").peek()) {
                for (auto& o : ofs->items()) {
                    if (o.value().is_number()) { namedOffsets[o.key()] = (double)o.value(); }
                }
            }
        }

        // Define custom offsets
        for (auto& [name, offset] : namedOffsets) {
            offsets.define(name, offsets.size());
        }
    }

    void init() {
        // Load offset modes
        reloadOffsets();

        // Define decimation values
        decimations.define(1, "None", 1);
        decimations.define(2, "2x", 2);
        decimations.define(4, "4x", 4);
        decimations.define(8, "8x", 8);
        decimations.define(16, "16x", 16);
        decimations.define(32, "32x", 32);
        decimations.define(64, "64x", 64);

        // Load other settings. Read under one access and applied after it
        // closes: selecting a source runs module code that reaches for the config.
        std::string selectedSource;
        std::string selectedOffset;
        int decimation = 1;
        {
            auto configAccess = core::configManager.read();
            configAccess.tryGet("source", selectedSource);
            configAccess.tryGet("manualOffset", manualOffset);
            configAccess.tryGet("selectedOffset", selectedOffset);
            configAccess.tryGet("iqCorrection", iqCorrection);
            configAccess.tryGet("invertIQ", invertIQ);
            configAccess.tryGet("decimation", decimation);
        }
        if (decimations.keyExists(decimation)) {
            decimId = decimations.keyId(decimation);
        }

        // Select the source module
        refreshSources();
        selectSource(selectedSource);

        // Update frontend settings
        sigpath::iqFrontEnd.setDCBlocking(iqCorrection);
        sigpath::iqFrontEnd.setInvertIQ(invertIQ);
        sigpath::iqFrontEnd.setDecimation(decimations.value(decimId));
        selectOffsetByName(selectedOffset);

        // Register handlers
        sourcesChangedHandler.handler = onSourcesChanged;
        sourceUnregisterHandler.handler = onSourceUnregister;
        sigpath::sourceManager.onSourceRegistered.bindHandler(&sourcesChangedHandler);
        sigpath::sourceManager.onSourceUnregister.bindHandler(&sourceUnregisterHandler);
        sigpath::sourceManager.onSourceUnregistered.bindHandler(&sourcesChangedHandler);
    }

    void addOffset(const std::string& name, double offset) {
        // Define a new offset
        core::configManager.edit().section("offsets").set(name, offset);

        // Reload the offsets
        reloadOffsets();

        // Attempt to re-select the same one
        selectOffsetByName(selectedOffset);
    }

    void delOffset(const std::string& name) {
        // Drop the offset
        core::configManager.edit().section("offsets").erase(name);

        // Reload the offsets
        reloadOffsets();

        // Attempt to re-select the same one
        selectOffsetByName(selectedOffset);
    }

    void addOffsetDialog() {
        gui::mainWindow.lockWaterfallControls = true;

        const char* id = "Add offset##sdrpp_add_offset_dialog_";
        if (addOffsetDlg.begin(id, ImGuiWindowFlags_NoResize)) {
            ImGui::LeftLabelFill("Name");
            ImGui::InputText("##sdrpp_add_offset_name", newOffsetName, 1023);

            ImGui::LeftLabelFill("Offset");
            ImGui::InputDouble("##sdrpp_add_offset_offset", &newOffset);

            bool nameExists = offsets.nameExists(newOffsetName);
            bool reservedName = !strcmp(newOffsetName, "None") || !strcmp(newOffsetName, "Manual");
            bool denyApply = !newOffsetName[0] || nameExists || reservedName;

            if (nameExists) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "An offset with the given name already exists.");
            }
            else if (reservedName) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "The given name is reserved.");
            }

            if (addOffsetDlg.applyButton("Apply", denyApply)) {
                addOffset(newOffsetName, newOffset);
                addOffsetDlg.close();
            }
            ImGui::SameLine();
            if (addOffsetDlg.cancelButton()) {
                addOffsetDlg.close();
            }
            addOffsetDlg.end();
        }
    }

    void draw(void* ctx) {
        float itemWidth = ImGui::GetContentRegionAvail().x;
        float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float spacing = lineHeight - ImGui::GetTextLineHeight();
        bool running = gui::mainWindow.sdrIsRunning();

        if (running) { style::beginDisabled(); }

        ImGui::SetNextItemWidth(itemWidth);
        if (ImGui::Combo("##source", &sourceId, sources.txt)) {
            std::string newSource = sources.value(sourceId);
            selectSource(newSource);
            core::configManager.edit().set("source", newSource);
        }

        if (running) { style::endDisabled(); }

        // Every networked source's receivers on one map; Connect stops the
        // radio itself, so this stays enabled while playing.
        if (ImGui::Button("Browse remote receivers...##_sdrpp_remote_browser", ImVec2(itemWidth, 0))) {
            remote::openBrowser();
        }

        sigpath::sourceManager.showSelectedMenu();

        const std::string& spectrumNotice = gui::mainWindow.spectrumFallbackNotice();
        if (!spectrumNotice.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
            ImGui::TextWrapped("%s", spectrumNotice.c_str());
            ImGui::PopStyleColor();
        }

        if (ImGui::Checkbox("IQ Correction##_sdrpp_iq_corr", &iqCorrection)) {
            sigpath::iqFrontEnd.setDCBlocking(iqCorrection);
            core::configManager.edit().set("iqCorrection", iqCorrection);
        }

        if (ImGui::Checkbox("Invert IQ##_sdrpp_inv_iq", &invertIQ)) {
            sigpath::iqFrontEnd.setInvertIQ(invertIQ);
            core::configManager.edit().set("invertIQ", invertIQ);
        }

        ImGui::LeftLabel("Offset mode");
        ImGui::SetNextItemWidth(itemWidth - ImGui::GetCursorPosX() - 2.0f*(lineHeight + 1.5f*spacing));
        if (ImGui::Combo("##_sdrpp_offset", &offsetId, offsets.txt)) {
            selectOffsetById(offsetId);
            core::configManager.edit().set("selectedOffset", offsets.key(offsetId));
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - spacing);
        if (offsetId < OFFSET_ID_CUSTOM_BASE) { ImGui::BeginDisabled(); }
        if (ImGui::Button("-##_sdrpp_offset_del_", ImVec2(lineHeight + 0.5f*spacing, 0))) {
            delOffsetName = selectedOffset;
            showDelOffsetDialog = true;
        }
        if (offsetId < OFFSET_ID_CUSTOM_BASE) { ImGui::EndDisabled(); }
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - spacing);
        if (ImGui::Button("+##_sdrpp_offset_add_", ImVec2(lineHeight + 0.5f*spacing, 0))) {
            strcpy(newOffsetName, "New Offset");
            addOffsetDlg.request();
        }

        // Offset delete confirmation
        if (ImGui::GenericDialog("sdrpp_del_offset_confirm", showDelOffsetDialog, GENERIC_DIALOG_BUTTONS_YES_NO, []() {
            ImGui::Text("Deleting offset named \"%s\". Are you sure?", delOffsetName.c_str());
        }) == GENERIC_DIALOG_BUTTON_YES) {
            delOffset(delOffsetName);
        }

        // Offset add diaglog
        if (addOffsetDlg.isOpen()) { addOffsetDialog(); }

        if (offsetId != OFFSET_ID_NONE) {
            ImGui::LeftLabel("Offset");
            ImGui::FillWidth();
            if (offsetId == OFFSET_ID_MANUAL) {
                if (ImGui::InputDouble("##freq_offset", &manualOffset, 1.0, 100.0)) {
                    updateOffset();
                    core::configManager.edit().set("manualOffset", manualOffset);
                }
            }
            else {
                style::beginDisabled();
                ImGui::InputDouble("##freq_offset", &effectiveOffset, 1.0, 100.0);
                style::endDisabled();
            }
        }

        if (running) { style::beginDisabled(); }
        ImGui::LeftLabel("Decimation");
        ImGui::FillWidth();
        if (ImGui::Combo("##source_decim", &decimId, decimations.txt)) {
            sigpath::iqFrontEnd.setDecimation(decimations.value(decimId));
            core::configManager.edit().set("decimation", decimations.key(decimId));
        }
        if (running) { style::endDisabled(); }
    }
}
