#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <thread>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <vector>
#include <gui/tuner.h>
#include <gui/file_dialogs.h>
#include <utils/freq_formatting.h>
#include <gui/dialogs/dialog_box.h>
#include <gui/widgets/popup_dialog.h>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "bookmark.h"
#include "schedule.h"

SDRPP_MOD_INFO{
    /* Name:            */ "frequency_manager",
    /* Description:     */ "Frequency manager module for SDRIAK",
    /* Author:          */ "Ryzerth;Zimm;Darau Ble;Davide Rovelli",
    /* Version:         */ 0, 4, 0,
    /* Max instances    */ 1
};

// Maximum number of waterfall label rows (bookmarkRows is an index 0..MAX_ROWS-1 meaning 1..MAX_ROWS rows)
constexpr int MAX_ROWS = 10;

ConfigManager config;

enum {
    BOOKMARK_DISP_MODE_OFF,
    BOOKMARK_DISP_MODE_TOP,
    BOOKMARK_DISP_MODE_BOTTOM,
    _BOOKMARK_DISP_MODE_COUNT
};

const char* bookmarkDisplayModesTxt = "Off\0Top\0Bottom\0";
const char* bookmarkRowsTxt = "1\0""2\0""3\0""4\0""5\0""6\0""7\0""8\0""9\0""10\0";

class FrequencyManagerModule : public ModuleManager::Instance {
public:
    FrequencyManagerModule(std::string name) {
        this->name = name;

        std::string selList;
        {
            auto configAccess = config.read();
            configAccess.tryGet("selectedList", selList);
            configAccess.tryGet("bookmarkDisplayMode", bookmarkDisplayMode);
            if (configAccess.tryGet("bookmarkRows", bookmarkRows)) {
                bookmarkRows = std::clamp<int>(bookmarkRows, 0, MAX_ROWS - 1);
            }
            configAccess.tryGet("bookmarkRectangle", bookmarkRectangle);
            configAccess.tryGet("bookmarkCentered", bookmarkCentered);
            configAccess.tryGet("bookmarkNoClutter", bookmarkNoClutter);
        }

        refreshLists();
        loadByName(selList);
        refreshWaterfallBookmarks();

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);
    }

    ~FrequencyManagerModule() {
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.unbindHandler(&inputHandler);
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void applyBookmark(FrequencyBookmark bm, std::string vfoName) {
        if (vfoName == "") {
            // TODO: Replace with proper tune call
            gui::waterfall.setCenterFrequency(bm.frequency);
            gui::waterfall.centerFreqMoved = true;
        }
        else {
            if (core::modComManager.interfaceExists(vfoName)) {
                if (core::modComManager.getModuleName(vfoName) == "radio") {
                    // Unrecorded fields are left alone rather than imposed. The
                    // bandwidth guard is not cosmetic: setBandwidth() clamps a 0
                    // to the demodulator's minimum and *persists* it into
                    // radio_config.json, so recalling a bookmark taken without a
                    // radio VFO would destroy the saved bandwidth of whatever
                    // mode it had just switched to.
                    if (bm.mode >= 0) {
                        int mode = bm.mode;
                        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    }
                    if (bm.bandwidth > 0) {
                        float bandwidth = bm.bandwidth;
                        core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                    }
                }
            }
            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, bm.frequency);
        }
    }

    // Bandwidth and mode captured from the currently selected VFO, used to seed
    // a freshly created bookmark. Either can come back unrecorded: mode -1 when
    // the VFO is not a radio (or there is none), bandwidth 0 when there is no
    // VFO at all. applyBookmark() then leaves that setting alone, so a bookmark
    // only ever imposes what was actually observed. RAW is deliberately not used
    // as the unknown value -- it is a real demodulator, and a bookmark asking
    // for it must be one the user chose.
    void currentBookmarkBwMode(double& bandwidth, int& mode) {
        bandwidth = 0;
        mode = -1;
        if (gui::waterfall.selectedVFO == "") { return; }
        bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
        if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
            // Seeded rather than merely assigned to: callInterface() invokes no
            // handler at all when the interface is not registered, and the radio
            // ignores the command while it has no demodulator selected. Reading
            // an uninitialized `m` in either case stored stack garbage as the
            // bookmark's mode.
            int m = -1;
            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &m);
            mode = m;
        }
    }

    // Seed editedBookmark for a brand new bookmark and open the edit dialog in
    // Create mode. Shared by the "Add" button and the right-click-on-spectrum
    // gesture.
    void beginCreateBookmark(double frequency, double bandwidth, int mode) {
        editedBookmark.frequency = frequency;
        editedBookmark.bandwidth = bandwidth;
        editedBookmark.mode = mode;
        editedBookmark.startTime = 0;
        editedBookmark.endTime = 0;
        for (int i = 0; i < 7; i++) { editedBookmark.days[i] = true; }
        editedBookmark.geoinfo = "";
        editedBookmark.notes = "";
        editedBookmark.selected = false;
        editedBookmarkListId = selectedListId;
        firstEditedBookmarkName = "";

        bookmarkDialogMode = BookmarkDialogMode::Create;
        editDialog.request();

        // Find a unique default name
        if (bookmarks.find("New Bookmark") == bookmarks.end()) {
            editedBookmarkName = "New Bookmark";
        }
        else {
            char buf[64];
            for (int i = 1; i < 1000; i++) {
                sprintf(buf, "New Bookmark (%d)", i);
                if (bookmarks.find(buf) == bookmarks.end()) { break; }
            }
            editedBookmarkName = buf;
        }
    }

    // Expand this module's menu section so the create dialog is actually drawn:
    // the dialog is only rendered from menuHandler, which the menu skips while
    // the section is collapsed. Used when the dialog is triggered from outside
    // the menu (right-click on the spectrum).
    void ensureMenuExpanded() {
        for (auto& opt : gui::menu.order) {
            if (opt.name == name) {
                opt.open = true;
                break;
            }
        }
        gui::mainWindow.setFirstMenuRender();
    }

    void saveBookmarkEdit(const std::string& targetListName) {
        if (targetListName != selectedListName) {
            // Moving to another list: remove from the current one if editing
            if (bookmarkDialogMode == BookmarkDialogMode::Edit) {
                bookmarks.erase(firstEditedBookmarkName);
                saveByName(selectedListName);
            }

            // Add to the target list and switch to it
            {
                config.edit()
                    .section("lists", targetListName, "bookmarks")
                    .set(editedBookmarkName, bookmarkToJson(editedBookmark));
            }
            refreshWaterfallBookmarks();

            loadByName(targetListName);
            config.edit().set("selectedList", targetListName);
        }
        else {
            // Same list: normal add or edit
            if (bookmarkDialogMode == BookmarkDialogMode::Edit) {
                bookmarks.erase(firstEditedBookmarkName);
            }
            bookmarks[editedBookmarkName] = editedBookmark;
            saveByName(selectedListName);
        }
    }

    void bookmarkEditDialog() {
        gui::mainWindow.lockWaterfallControls = true;

        std::string id = "Edit##freq_manager_edit_popup_" + name;
        if (editDialog.begin(id.c_str(), ImGuiWindowFlags_NoResize)) {
            char nameBuf[1024];
            snprintf(nameBuf, sizeof(nameBuf), "%s", editedBookmarkName.c_str());

            char geoinfoBuf[2048];
            snprintf(geoinfoBuf, sizeof(geoinfoBuf), "%s", editedBookmark.geoinfo.c_str());

            char notesBuf[4096];
            snprintf(notesBuf, sizeof(notesBuf), "%s", editedBookmark.notes.c_str());

            float editWinSize = 250.0f * style::uiScale;
            ImGui::BeginTable(("freq_manager_edit_table" + name).c_str(), 2);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Name");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            if (ImGui::InputText(("##freq_manager_edit_name" + name).c_str(), nameBuf, sizeof(nameBuf) - 1)) {
                editedBookmarkName = nameBuf;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("List");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            ImGui::Combo(("##freq_manager_edit_list" + name).c_str(), &editedBookmarkListId, listNamesTxt.c_str());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Frequency");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            ImGui::InputDouble(("##freq_manager_edit_freq" + name).c_str(), &editedBookmark.frequency);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Bandwidth");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            ImGui::InputDouble(("##freq_manager_edit_bw" + name).c_str(), &editedBookmark.bandwidth);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Mode");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            // BeginCombo rather than Combo: the preview is a separate parameter
            // from the item list, so "not set" can be shown as a prompt without
            // being an entry the user could choose. A bookmark reaches mode -1
            // by being taken without a radio VFO, or by carrying an unusable
            // value in the file -- never by selection, and picking a real mode
            // is one-way.
            {
                bool modeNotSet = !radioModeValid(editedBookmark.mode);
                const char* preview = modeNotSet ? "(not set)" : RADIO_IFACE_MODE_NAMES[editedBookmark.mode];
                if (modeNotSet) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); }
                bool modeOpen = ImGui::BeginCombo(("##freq_manager_edit_mode" + name).c_str(), preview);
                if (modeNotSet) { ImGui::PopStyleColor(); }
                if (modeOpen) {
                    for (int i = 0; i < _RADIO_IFACE_MODE_COUNT; i++) {
                        bool selected = (editedBookmark.mode == i);
                        if (ImGui::Selectable(RADIO_IFACE_MODE_NAMES[i], selected)) { editedBookmark.mode = i; }
                        if (selected) { ImGui::SetItemDefaultFocus(); }
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Start Time");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            ImGui::InputScalarN(
                ("##freq_manager_edit_start_time" + name).c_str(),
                ImGuiDataType_S32,
                &editedBookmark.startTime, 1,
                NULL, NULL, "%04d", 0);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("End Time");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            ImGui::InputScalarN(
                ("##freq_manager_edit_end_time" + name).c_str(),
                ImGuiDataType_S32,
                &editedBookmark.endTime, 1,
                NULL, NULL, "%04d", 0);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Days");
            ImGui::TableSetColumnIndex(1);

            ImGui::BeginGroup();
            ImGui::Columns(4, "FreqManagerBookmarkDays", false);
            ImGui::Checkbox("Su", &editedBookmark.days[0]);
            ImGui::Checkbox("Th", &editedBookmark.days[4]);
            ImGui::NextColumn();
            ImGui::Checkbox("Mo", &editedBookmark.days[1]);
            ImGui::Checkbox("Fr", &editedBookmark.days[5]);
            ImGui::NextColumn();
            ImGui::Checkbox("Tu", &editedBookmark.days[2]);
            ImGui::Checkbox("Sa", &editedBookmark.days[6]);
            ImGui::NextColumn();
            ImGui::Checkbox("We", &editedBookmark.days[3]);
            ImGui::Columns(1, "FreqManagerEndBookmarkDays", false);
            ImGui::EndGroup();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Geo Info");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            if (ImGui::InputText(("##freq_manager_edit_geoinfo" + name).c_str(), geoinfoBuf, sizeof(geoinfoBuf) - 1)) {
                editedBookmark.geoinfo = geoinfoBuf;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Notes");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(editWinSize);
            if (ImGui::InputTextMultiline(("##freq_manager_edit_notes" + name).c_str(), notesBuf, sizeof(notesBuf) - 1)) {
                editedBookmark.notes = notesBuf;
            }
            bool notesFieldActive = ImGui::IsItemActive();

            ImGui::EndTable();

            std::string targetListName = listNames.empty() ? selectedListName : listNames[std::clamp<int>(editedBookmarkListId, 0, listNames.size() - 1)];

            // Check for a name conflict in the target list
            bool nameExists;
            if (targetListName != selectedListName) {
                auto configAccess = config.read();
                nameExists = configAccess.section("lists", targetListName, "bookmarks").contains(editedBookmarkName);
            }
            else {
                nameExists = (bookmarks.find(editedBookmarkName) != bookmarks.end()) && (editedBookmarkName != firstEditedBookmarkName);
            }

            bool applyDisabled = (strlen(nameBuf) == 0) || nameExists || !timeValid(editedBookmark.startTime) || !timeValid(editedBookmark.endTime);
            if (editDialog.applyButton("Apply", applyDisabled, notesFieldActive)) {
                saveBookmarkEdit(targetListName);
                editDialog.close();
            }
            ImGui::SameLine();
            if (editDialog.cancelButton()) {
                editDialog.close();
            }
            editDialog.end();
        }
    }

    void newListDialog() {
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::string id = "New##freq_manager_new_popup_" + name;
        if (listDialog.begin(id.c_str(), ImGuiWindowFlags_NoResize)) {
            char nameBuf[1024];
            snprintf(nameBuf, sizeof(nameBuf), "%s", editedListName.c_str());

            ImGui::LeftLabel("Name");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##freq_manager_edit_name" + name).c_str(), nameBuf, sizeof(nameBuf) - 1)) {
                editedListName = nameBuf;
            }

            ImGui::LeftLabel("Color");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            ImGui::ColorEdit3(("##freq_manager_list_color_" + name).c_str(), (float*)&editedListColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

            bool nameChanged = (listDialogMode != ListDialogMode::Rename) || (editedListName != firstEditedListName);
            bool alreadyExists = nameChanged && (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());

            if (listDialog.applyButton("Apply", (strlen(nameBuf) == 0) || alreadyExists)) {
                {
                    auto configAccess = config.edit();
                    ConfigManager::EditSection lists = configAccess.section("lists");
                    if (listDialogMode == ListDialogMode::Rename) {
                        if (editedListName != firstEditedListName) {
                            lists.set(editedListName, lists.value(firstEditedListName, json::object()));
                            lists.erase(firstEditedListName);
                        }
                    }
                    else {
                        ConfigManager::EditSection list = lists.section(editedListName);
                        list.set("showOnWaterfall", true);
                        list.set("bookmarks", json::object());
                    }

                    char buf[16];
                    snprintf(buf, sizeof(buf), "#%02X%02X%02X", (int)roundf(editedListColor.x * 255), (int)roundf(editedListColor.y * 255), (int)roundf(editedListColor.z * 255));
                    lists.section(editedListName).set("color", buf);
                }

                refreshWaterfallBookmarks();
                refreshLists();
                loadByName(editedListName);
                listDialog.close();
            }
            ImGui::SameLine();
            if (listDialog.cancelButton()) {
                listDialog.close();
            }
            listDialog.end();
        }
    }

    void selectListsDialog() {
        gui::mainWindow.lockWaterfallControls = true;

        std::string id = "Select lists##freq_manager_sel_popup_" + name;
        if (selectDialog.begin(id.c_str(), ImGuiWindowFlags_NoResize)) {
            // Copied out so the checkboxes below can write without the lock held.
            json lists;
            {
                auto configAccess = config.read();
                lists = configAccess.value("lists", json::object());
            }
            for (auto [listName, list] : lists.items()) {
                bool shown = list.value("showOnWaterfall", true);
                if (ImGui::Checkbox((listName + "##freq_manager_sel_list_").c_str(), &shown)) {
                    {
                        config.edit().section("lists", listName).set("showOnWaterfall", shown);
                    }
                    refreshWaterfallBookmarks();
                }
            }

            // Checkbox changes apply immediately, so Escape and Ok both just dismiss
            if (selectDialog.applyButton("Ok") || selectDialog.cancelRequested()) {
                selectDialog.close();
            }
            selectDialog.end();
        }
    }

    void refreshLists() {
        listNames.clear();
        listNamesTxt = "";
        sortSpecsDirty = true;

        auto configAccess = config.read();
        const json* lists = configAccess.section("lists").peek();
        if (!lists) { return; }
        for (auto [_name, list] : lists->items()) {
            listNames.push_back(_name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
        }
    }

    // Takes the config lock itself, so it must be called with no access open.
    void refreshWaterfallBookmarks() {
        json lists;
        {
            auto configAccess = config.read();
            lists = configAccess.value("lists", json::object());
        }

        waterfallBookmarks.clear();
        for (auto [listName, list] : lists.items()) {
            if (!list.value("showOnWaterfall", false)) { continue; }
            WaterfallBookmark wbm;
            wbm.listName = listName;
            wbm.color = IM_COL32(255, 255, 0, 255);
            if (list.contains("color") && list["color"].is_string()) {
                wbm.color = hexStrToColor(list["color"]);
            }
            if (!list.contains("bookmarks")) { continue; }
            for (auto [bookmarkName, bm] : list["bookmarks"].items()) {
                wbm.bookmarkName = bookmarkName;
                wbm.bookmark = bookmarkFromJson(bm);
                wbm.clampedRectMin = ImVec2(-1, -1);
                wbm.clampedRectMax = ImVec2(-1, -1);
                waterfallBookmarks.push_back(wbm);
            }
        }
        std::sort(waterfallBookmarks.begin(), waterfallBookmarks.end(), compareWaterfallBookmarks);
    }

    void loadFirst() {
        if (listNames.size() > 0) {
            loadByName(listNames[0]);
            return;
        }
        selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName) {
        bookmarks.clear();
        sortSpecsDirty = true;
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end()) {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;

        json storedBookmarks;
        {
            auto configAccess = config.read();
            const json* bms = configAccess.section("lists", listName, "bookmarks").peek();
            if (bms) { storedBookmarks = *bms; }
        }
        if (!storedBookmarks.is_object()) { return; }
        for (auto [bmName, bm] : storedBookmarks.items()) {
            bookmarks[bmName] = bookmarkFromJson(bm);
        }
    }

    void saveByName(std::string listName) {
        json bms = json::object();
        for (auto [bmName, bm] : bookmarks) {
            bms[bmName] = bookmarkToJson(bm);
        }
        {
            config.edit().section("lists", listName).set("bookmarks", bms);
        }
        refreshWaterfallBookmarks();
        sortSpecsDirty = true;
    }

    static void menuHandler(void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // TODO: Replace with something that won't iterate every frame
        std::vector<std::string> selectedNames;
        for (auto& [name, bm] : _this->bookmarks) {
            if (bm.selected) { selectedNames.push_back(name); }
        }

        ImGuiStyle& imStyle = ImGui::GetStyle();
        float sqBtnSize = ImGui::GetFrameHeight();

        float btnSize = ImGui::CalcTextSize("Rename").x + (imStyle.FramePadding.x * 2.0f);
        ImGui::SetNextItemWidth(menuWidth - btnSize - (2.0f * sqBtnSize) - (3.0f * imStyle.ItemSpacing.x));
        if (ImGui::Combo(("##freq_manager_list_sel" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str())) {
            _this->loadByName(_this->listNames[_this->selectedListId]);
            config.edit().set("selectedList", _this->selectedListName);
        }
        ImGui::SameLine();
        if (_this->listNames.size() == 0) { style::beginDisabled(); }
        if (ImGui::Button(("Rename##_freq_mgr_ren_lst_" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            _this->firstEditedListName = _this->listNames[_this->selectedListId];
            _this->editedListName = _this->firstEditedListName;
            _this->editedListColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            {
                auto configAccess = config.read();
                std::string color;
                if (configAccess.section("lists", _this->firstEditedListName).tryGet("color", color)) {
                    _this->editedListColor = color32ToVec4(hexStrToColor(color));
                }
            }
            _this->listDialogMode = ListDialogMode::Rename;
            _this->listDialog.request();
        }
        if (_this->listNames.size() == 0) { style::endDisabled(); }
        ImGui::SameLine();
        if (ImGui::Button(("+##_freq_mgr_add_lst_" + _this->name).c_str(), ImVec2(sqBtnSize, 0))) {
            // Find new unique default name
            if (std::find(_this->listNames.begin(), _this->listNames.end(), "New List") == _this->listNames.end()) {
                _this->editedListName = "New List";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "New List (%d)", i);
                    if (std::find(_this->listNames.begin(), _this->listNames.end(), buf) == _this->listNames.end()) { break; }
                }
                _this->editedListName = buf;
            }
            _this->editedListColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            _this->listDialogMode = ListDialogMode::New;
            _this->listDialog.request();
        }
        ImGui::SameLine();
        if (_this->selectedListName == "") { style::beginDisabled(); }
        if (ImGui::Button(("-##_freq_mgr_del_lst_" + _this->name).c_str(), ImVec2(sqBtnSize, 0))) {
            _this->deleteListOpen = true;
        }
        if (_this->selectedListName == "") { style::endDisabled(); }

        // List delete confirmation
        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::Text("Deleting list named \"%s\". Are you sure?", _this->selectedListName.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            {
                config.edit().section("lists").erase(_this->selectedListName);
            }
            _this->refreshWaterfallBookmarks();
            _this->refreshLists();
            if (_this->listNames.size() > 0) {
                _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, (int)_this->listNames.size() - 1);
                _this->loadByName(_this->listNames[_this->selectedListId]);
            }
            else {
                _this->selectedListId = 0;
                _this->selectedListName = "";
            }
        }

        if (_this->selectedListName == "") { style::beginDisabled(); }
        //Draw buttons on top of the list
        ImGui::BeginTable(("freq_manager_btn_table" + _this->name).c_str(), 3, 0, ImVec2(ImGui::BeginActionRow(), 0));
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Add##_freq_mgr_add_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            // If there's no VFO selected, just save the center freq
            double bw;
            int mode;
            _this->currentBookmarkBwMode(bw, mode);
            double freq = (gui::waterfall.selectedVFO == "")
                              ? gui::waterfall.getCenterFrequency()
                              : gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
            _this->beginCreateBookmark(freq, bw, mode);
        }

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Remove##_freq_mgr_rem_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->deleteBookmarksOpen = true;
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::TableSetColumnIndex(2);
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Edit##_freq_mgr_edt_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->bookmarkDialogMode = BookmarkDialogMode::Edit;
            _this->editDialog.request();
            _this->editedBookmark = _this->bookmarks[selectedNames[0]];
            _this->editedBookmarkName = selectedNames[0];
            _this->firstEditedBookmarkName = selectedNames[0];
            _this->editedBookmarkListId = _this->selectedListId;
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        ImGui::EndTable();

        // Bookmark delete confirm dialog
        if (ImGui::GenericDialog(("freq_manager_del_bkm_confirm" + _this->name).c_str(), _this->deleteBookmarksOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::TextUnformatted("Deleting selected bookmarks. Are you sure?");
            }) == GENERIC_DIALOG_BUTTON_YES) {
            for (auto& _name : selectedNames) { _this->bookmarks.erase(_name); }
            _this->saveByName(_this->selectedListName);
        }

        // Bookmark list
        if (ImGui::BeginTable(("freq_manager_bkm_table" + _this->name).c_str(), 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable, ImVec2(0, 200.0f * style::uiScale))) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
            ImGui::TableSetupColumn("Bookmark", ImGuiTableColumnFlags_DefaultSort, 0.0f, 1);
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();

            // Re-sort when the user clicks a column header or the bookmark list changed
            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (sortSpecs && (sortSpecs->SpecsDirty || _this->sortSpecsDirty)) {
                _this->sortedBookmarks.assign(_this->bookmarks.begin(), _this->bookmarks.end());
                _this->scrollToClickedBookmark = true;
                if (sortSpecs->SpecsCount > 0) {
                    ImGuiTableColumnSortSpecs spec = sortSpecs->Specs[0];
                    if (spec.ColumnUserID == 0) {
                        // Sort by name (map iteration is already name-ascending)
                        if (spec.SortDirection == ImGuiSortDirection_Descending) {
                            std::reverse(_this->sortedBookmarks.begin(), _this->sortedBookmarks.end());
                        }
                    }
                    else {
                        // Sort by frequency
                        std::sort(_this->sortedBookmarks.begin(), _this->sortedBookmarks.end(),
                                  (spec.SortDirection == ImGuiSortDirection_Descending) ? compareBookmarksFreqDesc : compareBookmarksFreqAsc);
                    }
                }
                sortSpecs->SpecsDirty = false;
                _this->sortSpecsDirty = false;
            }

            for (auto& [name, bm] : _this->sortedBookmarks) {
                auto it = _this->bookmarks.find(name);
                if (it == _this->bookmarks.end()) { continue; }
                FrequencyBookmark& cbm = it->second;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                if (ImGui::Selectable((name + "##_freq_mgr_bkm_name_" + _this->name).c_str(), &cbm.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) {
                    // if shift or control isn't pressed, deselect all others
                    if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
                        for (auto& [_name, _bm] : _this->bookmarks) {
                            if (name == _name) { continue; }
                            _bm.selected = false;
                        }
                    }
                }
                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    applyBookmark(cbm, gui::waterfall.selectedVFO);
                    cbm.selected = true;
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s %s", utils::formatFreq(cbm.frequency).c_str(), radioModeName(cbm.mode));

                if (_this->scrollToClickedBookmark && cbm.selected) {
                    ImGui::SetScrollHereY(0.5f);
                    _this->scrollToClickedBookmark = false;
                }
            }
            ImGui::EndTable();
        }


        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::ActionButton(("Apply##_freq_mgr_apply_" + _this->name).c_str())) {
            FrequencyBookmark& bm = _this->bookmarks[selectedNames[0]];
            applyBookmark(bm, gui::waterfall.selectedVFO);
            bm.selected = false;
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        //Draw import and export buttons
        ImGui::BeginTable(("freq_manager_bottom_btn_table" + _this->name).c_str(), 2, 0, ImVec2(ImGui::BeginActionRow(), 0));
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Import##_freq_mgr_imp_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen) {
            _this->importOpen = true;
            _this->importDialog = new pfd::open_file("Import bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" }, pfd::opt::multiselect);
        }

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Export##_freq_mgr_exp_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportOpen) {
            _this->exportedBookmarks = json::object();
            {
                auto configAccess = config.read();
                ConfigManager::ReadSection bms = configAccess.section("lists", _this->selectedListName, "bookmarks");
                for (auto& _name : selectedNames) {
                    _this->exportedBookmarks["bookmarks"][_name] = bms.value(_name, json::object());
                }
            }
            _this->exportOpen = true;
            _this->exportDialog = new pfd::save_file("Export bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" });
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::EndTable();

        if (ImGui::ActionButton(("Select displayed lists##_freq_mgr_exp_" + _this->name).c_str())) {
            _this->selectDialog.request();
        }

        ImGui::LeftLabel("Bookmark display mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##_freq_mgr_dms_" + _this->name).c_str(), &_this->bookmarkDisplayMode, bookmarkDisplayModesTxt)) {
            config.edit().set("bookmarkDisplayMode", _this->bookmarkDisplayMode);
        }

        ImGui::LeftLabel("Rows of bookmarks");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##_freq_mgr_rob_" + _this->name).c_str(), &_this->bookmarkRows, bookmarkRowsTxt)) {
            config.edit().set("bookmarkRows", _this->bookmarkRows);
        }

        if (ImGui::Checkbox(("Rectangles##_freq_mgr_rect_" + _this->name).c_str(), &_this->bookmarkRectangle)) {
            config.edit().set("bookmarkRectangle", _this->bookmarkRectangle);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(("Centered##_freq_mgr_cen_" + _this->name).c_str(), &_this->bookmarkCentered)) {
            config.edit().set("bookmarkCentered", _this->bookmarkCentered);
        }

        if (ImGui::Checkbox(("Avoid clutter on last row##_freq_mgr_noClut_" + _this->name).c_str(), &_this->bookmarkNoClutter)) {
            config.edit().set("bookmarkNoClutter", _this->bookmarkNoClutter);
        }

        if (_this->selectedListName == "") { style::endDisabled(); }

        if (_this->bookmarkDialogMode != BookmarkDialogMode::None) {
            _this->bookmarkEditDialog();
            if (!_this->editDialog.isOpen()) { _this->bookmarkDialogMode = BookmarkDialogMode::None; }
        }

        if (_this->listDialogMode != ListDialogMode::None) {
            _this->newListDialog();
            if (!_this->listDialog.isOpen()) { _this->listDialogMode = ListDialogMode::None; }
        }

        if (_this->selectDialog.isOpen()) {
            _this->selectListsDialog();
        }

        // Handle import and export
        if (_this->importOpen && _this->importDialog->ready()) {
            _this->importOpen = false;
            std::vector<std::string> paths = _this->importDialog->result();
            if (paths.size() > 0 && _this->listNames.size() > 0) {
                _this->importBookmarks(paths[0]);
            }
            delete _this->importDialog;
        }
        if (_this->exportOpen && _this->exportDialog->ready()) {
            _this->exportOpen = false;
            std::string path = _this->exportDialog->result();
            if (path != "") {
                _this->exportBookmarks(path);
            }
            delete _this->exportDialog;
        }
    }

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        // Rectangles already placed on each label row, for overlap avoidance
        std::vector<BookmarkRectangle> rowRects[MAX_ROWS];
        int lastRow = std::clamp<int>(_this->bookmarkRows, 0, MAX_ROWS - 1);

        int now = getUTCTime();
        int weekDay = getWeekDay();
        bool top = (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP);

        auto overlapsRow = [](const std::vector<BookmarkRectangle>& row, double minX, double maxX) {
            for (const auto& r : row) {
                if (minX <= r.max && maxX >= r.min) { return true; }
            }
            return false;
        };

        for (auto& bm : _this->waterfallBookmarks) {
            // Invalidate the cached hit-test rectangle; it's set again if the label is drawn
            bm.clampedRectMin = ImVec2(-1, -1);
            bm.clampedRectMax = ImVec2(-1, -1);

            if (bm.bookmark.frequency < args.lowFreq || bm.bookmark.frequency > args.highFreq) { continue; }

            double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
            ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());

            double bmMinX, bmMaxX;
            if (_this->bookmarkCentered) {
                bmMinX = centerXpos - (nameSize.x / 2) - 5;
                bmMaxX = centerXpos + (nameSize.x / 2) + 5;
            }
            else {
                bmMinX = centerXpos - 5;
                bmMaxX = centerXpos + nameSize.x + 5;
            }

            // Find the first row where the label doesn't overlap an already placed one
            int row = -1;
            for (int i = 0; i <= lastRow; i++) {
                if (!overlapsRow(rowRects[i], bmMinX, bmMaxX)) {
                    row = i;
                    break;
                }
            }
            if (row < 0) {
                // All rows collide: either skip the label or pile it up on the last row
                if (_this->bookmarkNoClutter) { continue; }
                row = lastRow;
            }

            // Skip labels that would end up outside of the FFT area
            float rowTop = top ? (args.min.y + (nameSize.y * row)) : (args.max.y - (nameSize.y * (row + 1)));
            float rowBottom = rowTop + nameSize.y;
            if (top && rowBottom >= args.max.y) { continue; }
            if (!top && rowTop <= args.min.y) { continue; }

            bm.clampedRectMin = ImVec2(std::clamp<double>(bmMinX, args.min.x, args.max.x), rowTop);
            bm.clampedRectMax = ImVec2(std::clamp<double>(bmMaxX, args.min.x, args.max.x), rowBottom);

            rowRects[row].push_back(BookmarkRectangle{ bmMinX, bmMaxX });

            ImU32 bookmarkColor = bm.color;
            if (!bookmarkOnline(bm.bookmark, now, weekDay)) {
                bookmarkColor = IM_COL32(128, 128, 128, 255);
            }
            ImU32 bookmarkTextColor = IM_COL32(0, 0, 0, 255);
            if (_this->bookmarkRectangle) {
                args.window->DrawList->AddRectFilled(bm.clampedRectMin, bm.clampedRectMax, bookmarkColor);
            }
            else {
                bookmarkTextColor = bookmarkColor;
            }

            if (top) {
                args.window->DrawList->AddLine(ImVec2(centerXpos, rowBottom), ImVec2(centerXpos, args.max.y), bookmarkColor);
            }
            else {
                args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, rowTop), bookmarkColor);
            }

            float textX = _this->bookmarkCentered ? (centerXpos - (nameSize.x / 2)) : (bmMinX + 6);
            if (textX >= args.min.x && (textX + nameSize.x) <= args.max.x) {
                args.window->DrawList->AddText(ImVec2(textX, rowTop), bookmarkTextColor, bm.bookmarkName.c_str());
            }
        }
    }

    bool mouseAlreadyDown = false;
    bool mouseClickedInLabel = false;
    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;

        // Right-click on the spectrum or waterfall: create a bookmark at the
        // frequency under the cursor. Independent of the bookmark display mode,
        // but needs a list to save into (matches the disabled "Add" button).
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && _this->selectedListName != "") {
            ImVec2 mp = ImGui::GetMousePos();
            bool inFFT = mp.x >= args.fftRectMin.x && mp.x < args.fftRectMax.x && mp.y >= args.fftRectMin.y && mp.y < args.fftRectMax.y;
            bool inWaterfall = mp.x >= args.waterfallRectMin.x && mp.x < args.waterfallRectMax.x && mp.y >= args.waterfallRectMin.y && mp.y < args.waterfallRectMax.y;
            if (inFFT || inWaterfall) {
                double freq = args.lowFreq + ((double)mp.x - args.fftRectMin.x) * args.pixelToFreqRatio;

                // Snap to the selected VFO's grid when there is one
                if (gui::waterfall.selectedVFO != "") {
                    double snap = gui::waterfall.vfos[gui::waterfall.selectedVFO]->snapInterval;
                    if (snap > 0) { freq = std::round(freq / snap) * snap; }
                }

                double bw;
                int mode;
                _this->currentBookmarkBwMode(bw, mode);
                _this->beginCreateBookmark(freq, bw, mode);
                _this->ensureMenuExpanded();

                gui::waterfall.inputHandled = true;
                return;
            }
        }

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->mouseClickedInLabel) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                _this->mouseClickedInLabel = false;
            }
            gui::waterfall.inputHandled = true;
            return;
        }

        // Check the label rectangles cached during the FFT redraw for a hovered bookmark
        bool inALabel = false;
        WaterfallBookmark hoveredBookmark;
        std::string hoveredBookmarkName;

        for (auto& bm : _this->waterfallBookmarks) {
            if (bm.clampedRectMax.x - bm.clampedRectMin.x <= 0) { continue; }
            if (ImGui::IsMouseHoveringRect(bm.clampedRectMin, bm.clampedRectMax)) {
                inALabel = true;
                hoveredBookmark = bm;
                hoveredBookmarkName = bm.bookmarkName;
            }
        }

        // Check if mouse was already down
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inALabel) {
            _this->mouseAlreadyDown = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _this->mouseAlreadyDown = false;
            _this->mouseClickedInLabel = false;
        }

        // If yes, cancel
        if (_this->mouseAlreadyDown || !inALabel) { return; }

        gui::waterfall.inputHandled = true;

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            _this->mouseClickedInLabel = true;
            applyBookmark(hoveredBookmark.bookmark, gui::waterfall.selectedVFO);

            // If the clicked bookmark belongs to another list, switch to it
            if (hoveredBookmark.listName != _this->selectedListName) {
                _this->loadByName(hoveredBookmark.listName);
                config.edit().set("selectedList", _this->selectedListName);
            }

            // Select the clicked bookmark in the manager list
            for (auto& [name, bm] : _this->bookmarks) {
                bm.selected = (name == hoveredBookmarkName);
            }
            _this->scrollToClickedBookmark = true;
        }

        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hoveredBookmarkName.c_str());
        ImGui::Separator();
        ImGui::Text("List: %s", hoveredBookmark.listName.c_str());
        ImGui::Text("Frequency: %s", utils::formatFreq(hoveredBookmark.bookmark.frequency).c_str());
        ImGui::Text("Bandwidth: %s", utils::formatFreq(hoveredBookmark.bookmark.bandwidth).c_str());
        ImGui::Text("Mode: %s", radioModeName(hoveredBookmark.bookmark.mode));
        if (hoveredBookmark.bookmark.startTime != 0 || hoveredBookmark.bookmark.endTime != 0) {
            ImGui::Text("Time: %04d - %04d UTC", hoveredBookmark.bookmark.startTime, hoveredBookmark.bookmark.endTime);
        }
        bool allDays = true;
        for (int i = 0; i < 7; i++) { allDays &= hoveredBookmark.bookmark.days[i]; }
        if (!allDays) {
            const char dayLetters[7] = { 'S', 'M', 'T', 'W', 'T', 'F', 'S' };
            char bookmarkDays[8];
            bookmarkDays[7] = 0;
            for (int i = 0; i < 7; i++) {
                bookmarkDays[i] = hoveredBookmark.bookmark.days[i] ? dayLetters[i] : '-';
            }
            ImGui::Text("Days: %s", bookmarkDays);
        }
        if (!hoveredBookmark.bookmark.geoinfo.empty()) {
            ImGui::Text("Geo info: %s", hoveredBookmark.bookmark.geoinfo.c_str());
        }
        if (!hoveredBookmark.bookmark.notes.empty()) {
            ImGui::Text("Notes: %s", hoveredBookmark.bookmark.notes.c_str());
        }
        ImGui::EndTooltip();
    }

    json exportedBookmarks;
    bool importOpen = false;
    bool exportOpen = false;
    pfd::open_file* importDialog;
    pfd::save_file* exportDialog;

    void importBookmarks(std::string path) {
        json importedBookmarks;
        try {
            std::ifstream fs(path);
            fs >> importedBookmarks;
        }
        catch (const std::exception& e) {
            flog::error("Could not parse bookmark file: {0}", e.what());
            return;
        }

        if (!importedBookmarks.contains("bookmarks")) {
            flog::error("File does not contain any bookmarks");
            return;
        }

        if (!importedBookmarks["bookmarks"].is_object()) {
            flog::error("Bookmark attribute is invalid");
            return;
        }

        // Load every bookmark
        int importedCount = 0;
        int invalidModeCount = 0;
        for (auto const [_name, bm] : importedBookmarks["bookmarks"].items()) {
            if (bookmarks.find(_name) != bookmarks.end()) {
                flog::warn("Bookmark with the name '{0}' already exists in list, skipping", _name);
                continue;
            }
            try {
                ExternalBookmarkParseResult parsed = bookmarkFromExternalJson(bm);
                bookmarks[_name] = parsed.bookmark;
                if (parsed.modeStatus == ExternalBookmarkModeStatus::Invalid) {
                    invalidModeCount++;
                    flog::warn(
                        "Bookmark '{0}' has an invalid or unsupported mode; "
                        "importing it without a mode",
                        _name);
                }
                importedCount++;
            }
            catch (const std::exception& e) {
                flog::warn("Could not import bookmark '{0}': {1}", _name, e.what());
            }
        }
        saveByName(selectedListName);

        flog::info("Imported {0} bookmarks", importedCount);
        if (invalidModeCount) {
            flog::warn("{0} imported bookmarks had no usable mode", invalidModeCount);
        }
    }

    void exportBookmarks(std::string path) {
        std::ofstream fs(path);
        fs << exportedBookmarks;
        fs.close();
    }

    std::string name;
    bool enabled = true;
    enum class BookmarkDialogMode { None, Create, Edit };
    BookmarkDialogMode bookmarkDialogMode = BookmarkDialogMode::None;
    PopupDialog editDialog;
    enum class ListDialogMode { None, New, Rename };
    ListDialogMode listDialogMode = ListDialogMode::None;
    PopupDialog listDialog;
    PopupDialog selectDialog;

    bool deleteListOpen = false;
    bool deleteBookmarksOpen = false;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;

    std::map<std::string, FrequencyBookmark> bookmarks;
    std::vector<std::pair<std::string, FrequencyBookmark>> sortedBookmarks;
    bool sortSpecsDirty = true;
    bool scrollToClickedBookmark = false;

    std::string editedBookmarkName = "";
    std::string firstEditedBookmarkName = "";
    FrequencyBookmark editedBookmark;
    int editedBookmarkListId = 0;

    std::vector<std::string> listNames;
    std::string listNamesTxt = "";
    std::string selectedListName = "";
    int selectedListId = 0;

    std::string editedListName;
    std::string firstEditedListName;
    ImVec4 editedListColor;

    std::vector<WaterfallBookmark> waterfallBookmarks;

    int bookmarkDisplayMode = 0;
    int bookmarkRows = 0;
    bool bookmarkRectangle = true;
    bool bookmarkCentered = true;
    bool bookmarkNoClutter = false;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["selectedList"] = "General";
    def["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    def["bookmarkRows"] = 4;
    def["bookmarkRectangle"] = true;
    def["bookmarkCentered"] = true;
    def["bookmarkNoClutter"] = false;
    def["lists"]["General"]["showOnWaterfall"] = true;
    def["lists"]["General"]["bookmarks"] = json::object();

    config.setPath(core::args["root"].s() + "/frequency_manager_config.json");
    config.load(def);
    config.enableAutoSave();

    // Fill in missing options and convert lists of the old type
    {
        auto configAccess = config.edit();
        configAccess.ensure("bookmarkDisplayMode", (int)BOOKMARK_DISP_MODE_TOP);
        configAccess.ensure("bookmarkRows", 4);
        configAccess.ensure("bookmarkRectangle", true);
        configAccess.ensure("bookmarkCentered", true);
        configAccess.ensure("bookmarkNoClutter", false);

        // A pre-0.2.6 list was the bookmark map itself; wrap it in the current
        // {showOnWaterfall, bookmarks} shape. Collected first, since the rewrite
        // would invalidate an iterator over the document.
        ConfigManager::EditSection lists = configAccess.section("lists");
        std::vector<std::pair<std::string, json>> legacy;
        if (const json* stored = lists.peek()) {
            for (auto [listName, list] : stored->items()) {
                if (list.contains("bookmarks") && list.contains("showOnWaterfall") && list["showOnWaterfall"].is_boolean()) { continue; }
                json newList = json::object();
                newList["showOnWaterfall"] = true;
                newList["bookmarks"] = list;
                legacy.emplace_back(listName, newList);
            }
        }
        for (auto& [listName, newList] : legacy) {
            lists.set(listName, newList);
        }
    }
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new FrequencyManagerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FrequencyManagerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
