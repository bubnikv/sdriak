#include <server.h>
#include "imgui.h"
#include <stdio.h>
#include <gui/main_window.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/icons.h>
#include <version.h>
#include <utils/flog.h>
#include <gui/widgets/bandplan.h>
#include <gui/widgets/band_stack.h>
#include <gui/widgets/freq_memory.h>
#include <stb_image.h>
#include <config.h>
#include <core.h>
#include <utils/curl_init.h>
#include <utils/proxy.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <gui/menus/theme.h>
#include <backend.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <utils/executable_path.h>

#ifndef INSTALL_PREFIX
#ifdef __APPLE__
#define INSTALL_PREFIX "/usr/local"
#else
#define INSTALL_PREFIX "/usr"
#endif
#endif

namespace core {
    ConfigManager configManager;
    ModuleManager moduleManager;
    ModuleComManager modComManager;
    CommandArgsParser args;

    bool saveState() {
        freq_memory::commitCurrent();
        return ConfigManager::flushAll();
    }

    // Whole span, no local zoom.
    static void resetWaterfallSpan(double span) {
        gui::waterfall.setBandwidth(span);
        gui::waterfall.setViewOffset(0);
        gui::waterfall.setViewBandwidth(span);
        gui::mainWindow.setViewBandwidthSlider(1.0);
    }

    void setInputSampleRate(double samplerate) {
        // Forward this to the server
        if (args["server"].b()) { server::setInputSampleRate(samplerate); return; }
        
        // Update IQ frontend input samplerate and get effective samplerate
        sigpath::iqFrontEnd.setSampleRate(samplerate);
        double effectiveSr  = sigpath::iqFrontEnd.getEffectiveSamplerate();
        
        // Reset zoom. A span set by the source replaces the rate as the
        // waterfall's width; the DSP still runs at the effective rate.
        double span = sigpath::iqFrontEnd.displaySpanOverride();
        resetWaterfallSpan((span > 0.0) ? span : effectiveSr);

        // Debug logs
        flog::info("New DSP samplerate: {0} (source samplerate is {1})", effectiveSr, samplerate);
    }

    void setDisplaySpan(double spanHz) {
        if (args["server"].b() || !(spanHz > 0.0)) { return; }
        const bool changed = (spanHz != gui::waterfall.getBandwidth());
        sigpath::iqFrontEnd.setDisplaySpanOverride(spanHz);
        resetWaterfallSpan(spanHz);
        gui::mainWindow.displaySpanConfirmed(spanHz);
        gui::mainWindow.clampVFOsToSpan();
        if (changed) { gui::waterfall.resetHistory(); }
    }

    void clearDisplaySpan() {
        if (args["server"].b() || sigpath::iqFrontEnd.displaySpanOverride() == 0.0) { return; }
        sigpath::iqFrontEnd.setDisplaySpanOverride(0.0);
        const double effectiveSr = sigpath::iqFrontEnd.getEffectiveSamplerate();
        const bool changed = (effectiveSr != gui::waterfall.getBandwidth());
        resetWaterfallSpan(effectiveSr);
        gui::mainWindow.clampVFOsToSpan();
        if (changed) { gui::waterfall.resetHistory(); }
    }

    // Relative paths from the config (e.g. "./modules" on Windows, "../Plugins"
    // in the MacOS bundle) are interpreted relative to the executable's
    // directory, not the working directory, so the app behaves the same no
    // matter where it is launched from.
    std::string resolveConfigPath(const std::string& path) {
        std::filesystem::path p(path);
        if (!p.is_absolute()) { p = getExecutableDirectory() / p; }
        return p.lexically_normal().string();
    }

    std::string getModulesDirectory() {
#if defined(__linux__) && defined(BUILD_APPIMAGE)
        if (const char* appdir = getenv("APPDIR")) {
            return std::string(appdir) + "/usr/lib/sdriak/plugins";
        }
#endif
        return resolveConfigPath(core::configManager.read().value("modulesDirectory", std::string()));
    }

    std::string getResourcesDirectory() {
#if defined(__linux__) && defined(BUILD_APPIMAGE)
        if (const char* appdir = getenv("APPDIR")) {
            return std::string(appdir) + "/usr/share/sdriak";
        }
#endif
        return resolveConfigPath(core::configManager.read().value("resourcesDirectory", std::string()));
    }
};

// main
int sdrpp_main(int argc, char* argv[]) {
#ifdef _WIN32
    // The UTF-8 activeCodePage manifest makes narrow strings UTF-8 process-wide;
    // switch the console codepage so log output renders correctly on legacy conhost.
    SetConsoleOutputCP(CP_UTF8);
#endif

    flog::info("SDRIAK v" VERSION_STR);

    // Define command line options and parse arguments
    core::args.defineAll();
    if (core::args.parse(argc, argv) < 0) { return -1; } 

    // Show help and exit if requested
    if (core::args["help"].b()) {
        core::args.showHelp();
        return 0;
    }

    // Initialize libcurl process-wide before anything spawns threads or makes
    // HTTP/WS calls. Plugins must not call curl_global_init themselves.
    curl::init();

    bool serverMode = (bool)core::args["server"];

#ifdef _WIN32
    // Free console if the user hasn't asked for a console and not in server mode
    if (!core::args["con"].b() && !serverMode) { FreeConsole(); }

    // Set error mode to avoid abnoxious popups
    SetErrorMode(SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif

    // Check root directory
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root)) {
        flog::warn("Root directory {0} does not exist, creating it", root);
        if (!std::filesystem::create_directories(root)) {
            flog::error("Could not create root directory {0}", root);
            return -1;
        }
    }

    // Check that the path actually is a directory
    if (!std::filesystem::is_directory(root)) {
        flog::error("{0} is not a directory", root);
        return -1;
    }

    // ======== DEFAULT CONFIG ========
    constexpr int CURRENT_CONFIG_VERSION = 1;
    json defConfig;
    // Increment when a released core config needs an explicit migration. Nested
    // subsystems may carry their own versions as frequencyMemory does below.
    defConfig["configVersion"] = CURRENT_CONFIG_VERSION;
    defConfig["bandColors"]["amateur"] = "#FF0000FF";
    defConfig["bandColors"]["aviation"] = "#00FF00FF";
    defConfig["bandColors"]["broadcast"] = "#0000FFFF";
    defConfig["bandColors"]["marine"] = "#00FFFFFF";
    defConfig["bandColors"]["military"] = "#FFFF00FF";
    defConfig["bandPlan"] = "General";
    defConfig["bandPlanEnabled"] = true;
    defConfig["bandPlanPos"] = 0;
    defConfig["centerTuning"] = false;
    defConfig["colorMap"] = "Classic";
    defConfig["fftHold"] = false;
    defConfig["fftHoldSpeed"] = 60;
    defConfig["fftSmoothing"] = false;
    defConfig["fftSmoothingSpeed"] = 100;
    defConfig["fastFFT"] = false;
    defConfig["fftHeight"] = 300;
    defConfig["fftHeightLogical"] = true;
    defConfig["fftRate"] = 20;
    defConfig["fftSize"] = 65536;
    defConfig["fftWindow"] = "Nuttall";
    defConfig["frequency"] = 100000000.0;
    defConfig[freq_memory::ROOT] = freq_memory::defaults();
    defConfig["fullWaterfallUpdate"] = false;
    defConfig["max"] = 0.0;
    defConfig["maximized"] = false;
    defConfig["fullscreen"] = false;

    // Menu
    defConfig["menuElements"] = json::array();

    defConfig["menuElements"][0]["name"] = "Source";
    defConfig["menuElements"][0]["open"] = true;

    defConfig["menuElements"][1]["name"] = "Radio";
    defConfig["menuElements"][1]["open"] = true;

    defConfig["menuElements"][2]["name"] = "Recorder";
    defConfig["menuElements"][2]["open"] = true;

    defConfig["menuElements"][3]["name"] = "Sinks";
    defConfig["menuElements"][3]["open"] = true;

    defConfig["menuElements"][4]["name"] = "Frequency Manager";
    defConfig["menuElements"][4]["open"] = true;

    defConfig["menuElements"][5]["name"] = "VFO Color";
    defConfig["menuElements"][5]["open"] = true;

    defConfig["menuElements"][6]["name"] = "Band Plan";
    defConfig["menuElements"][6]["open"] = true;

    defConfig["menuElements"][7]["name"] = "Display";
    defConfig["menuElements"][7]["open"] = true;

    defConfig["menuWidth"] = 300;
    defConfig["menuWidthLogical"] = true;
    defConfig["min"] = -70.0;

    // Module instances
    defConfig["moduleInstances"]["Airspy Source"]["module"] = "airspy_source";
    defConfig["moduleInstances"]["Airspy Source"]["enabled"] = true;
    defConfig["moduleInstances"]["AirspyHF+ Source"]["module"] = "airspyhf_source";
    defConfig["moduleInstances"]["AirspyHF+ Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Audio Source"]["module"] = "audio_source";
    defConfig["moduleInstances"]["Audio Source"]["enabled"] = true;
    defConfig["moduleInstances"]["BladeRF Source"]["module"] = "bladerf_source";
    defConfig["moduleInstances"]["BladeRF Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Dragon Labs Source"]["module"] = "dragonlabs_source";
    defConfig["moduleInstances"]["Dragon Labs Source"]["enabled"] = true;
    defConfig["moduleInstances"]["File Source"]["module"] = "file_source";
    defConfig["moduleInstances"]["File Source"]["enabled"] = true;
    defConfig["moduleInstances"]["FobosSDR Source"]["module"] = "fobossdr_source";
    defConfig["moduleInstances"]["FobosSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["HackRF Source"]["module"] = "hackrf_source";
    defConfig["moduleInstances"]["HackRF Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Harogic Source"]["module"] = "harogic_source";
    defConfig["moduleInstances"]["Harogic Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Hermes Source"]["module"] = "hermes_source";
    defConfig["moduleInstances"]["Hermes Source"]["enabled"] = true;
    defConfig["moduleInstances"]["HydraSDR Source"]["module"] = "hydrasdr_source";
    defConfig["moduleInstances"]["HydraSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["KiwiSDR Source"]["module"] = "kiwisdr_source";
    defConfig["moduleInstances"]["KiwiSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["LimeSDR Source"]["module"] = "limesdr_source";
    defConfig["moduleInstances"]["LimeSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Network Source"]["module"] = "network_source";
    defConfig["moduleInstances"]["Network Source"]["enabled"] = true;
    defConfig["moduleInstances"]["OpenWebRX+ Source"]["module"] = "openwebrx_source";
    defConfig["moduleInstances"]["OpenWebRX+ Source"]["enabled"] = true;
    defConfig["moduleInstances"]["PerseusSDR Source"]["module"] = "perseus_source";
    defConfig["moduleInstances"]["PerseusSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["PlutoSDR Source"]["module"] = "plutosdr_source";
    defConfig["moduleInstances"]["PlutoSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["QMX Source"]["module"] = "qmx_source";
    defConfig["moduleInstances"]["QMX Source"]["enabled"] = true;
    defConfig["moduleInstances"]["QMX Server Source"]["module"] = "qmxserver_source";
    defConfig["moduleInstances"]["QMX Server Source"]["enabled"] = true;
    defConfig["moduleInstances"]["RFNM Source"]["module"] = "rfnm_source";
    defConfig["moduleInstances"]["RFNM Source"]["enabled"] = true;
    defConfig["moduleInstances"]["RFspace Source"]["module"] = "rfspace_source";
    defConfig["moduleInstances"]["RFspace Source"]["enabled"] = true;
    defConfig["moduleInstances"]["RTL-SDR Source"]["module"] = "rtl_sdr_source";
    defConfig["moduleInstances"]["RTL-SDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["RTL-TCP Source"]["module"] = "rtl_tcp_source";
    defConfig["moduleInstances"]["RTL-TCP Source"]["enabled"] = true;
    defConfig["moduleInstances"]["SDRplay Source"]["module"] = "sdrplay_source";
    defConfig["moduleInstances"]["SDRplay Source"]["enabled"] = true;
    defConfig["moduleInstances"]["SDRIAK Server Source"]["module"] = "sdriak_server_source";
    defConfig["moduleInstances"]["SDRIAK Server Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Spectran HTTP Source"]["module"] = "spectran_http_source";
    defConfig["moduleInstances"]["Spectran HTTP Source"]["enabled"] = true;
    defConfig["moduleInstances"]["SpyServer Source"]["module"] = "spyserver_source";
    defConfig["moduleInstances"]["SpyServer Source"]["enabled"] = true;
    defConfig["moduleInstances"]["UberSDR Source"]["module"] = "ubersdr_source";
    defConfig["moduleInstances"]["UberSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["USRP Source"]["module"] = "usrp_source";
    defConfig["moduleInstances"]["USRP Source"]["enabled"] = true;

    defConfig["moduleInstances"]["Audio Sink"] = "audio_sink";
    defConfig["moduleInstances"]["Network Sink"] = "network_sink";

    defConfig["moduleInstances"]["Radio"] = "radio";
    defConfig["moduleInstances"]["Radiosonde Decoder"]["module"] = "radiosonde_decoder";
    defConfig["moduleInstances"]["Radiosonde Decoder"]["enabled"] = false;

    defConfig["moduleInstances"]["Frequency Manager"] = "frequency_manager";
    defConfig["moduleInstances"]["Recorder"] = "recorder";
    defConfig["moduleInstances"]["Rigctl Server"] = "rigctl_server";
    defConfig["moduleInstances"]["Noise Reduction"]["module"] = "noise_reduction_logmmse";
    defConfig["moduleInstances"]["Noise Reduction"]["enabled"] = false;
    defConfig["moduleInstances"]["Spots"]["module"] = "spots";
    defConfig["moduleInstances"]["Spots"]["enabled"] = false;
    defConfig["moduleInstances"]["WebSDR View"]["module"] = "websdr_view";
    defConfig["moduleInstances"]["WebSDR View"]["enabled"] = false;
    // defConfig["moduleInstances"]["Rigctl Client"] = "rigctl_client";
    // TODO: Enable rigctl_client when ready
    // defConfig["moduleInstances"]["Scanner"] = "scanner";
    // TODO: Enable scanner when ready

    // Themes
    defConfig["theme"] = "Dark";
    defConfig["uiScaleFactor"] = 1.0f;
    defConfig["touchStyle"] = style::touchStyle;

    defConfig["modules"] = json::array();

    defConfig["offsets"]["SpyVerter"] = 120000000.0;
    defConfig["offsets"]["Ham-It-Up"] = 125000000.0;
    defConfig["offsets"]["MMDS S-band (1998MHz)"] = -1998000000.0;
    defConfig["offsets"]["DK5AV X-Band"] = -6800000000.0;
    defConfig["offsets"]["Ku LNB (9750MHz)"] = -9750000000.0;
    defConfig["offsets"]["Ku LNB (10700MHz)"] = -10700000000.0;

    defConfig["selectedOffset"] = "None";
    defConfig["manualOffset"] = 0.0;
    defConfig["showMenu"] = true;
    defConfig["showWaterfall"] = true;
    defConfig["waterfallAutoRange"] = false;
    defConfig["source"] = "";
    defConfig["decimation"] = 1;
    defConfig["iqCorrection"] = false;
    defConfig["invertIQ"] = false;

    defConfig["streams"]["Radio"]["muted"] = false;
    defConfig["streams"]["Radio"]["sink"] = "Audio";
    defConfig["streams"]["Radio"]["volume"] = 1.0f;

    defConfig["windowSize"]["h"] = 720;
    defConfig["windowSize"]["w"] = 1280;

    defConfig["vfoOffsets"] = json::object();

    defConfig["vfoColors"]["Radio"] = "#FFFFFF";

    // Proxy for every libcurl handle (utils/proxy.h); the Network menu edits it.
    defConfig["network"]["proxyMode"] = "system";
    defConfig["network"]["proxyUrl"] = "";
    defConfig["network"]["proxyUser"] = "";
    defConfig["network"]["proxyPassword"] = "";
    defConfig["network"]["noProxy"] = "";

#ifdef __ANDROID__
    defConfig["lockMenuOrder"] = true;
#else
    defConfig["lockMenuOrder"] = false;
#endif

#if defined(_WIN32)
    defConfig["modulesDirectory"] = "./modules";
    defConfig["resourcesDirectory"] = "./res";
#elif defined(IS_MACOS_BUNDLE)
    defConfig["modulesDirectory"] = "../Plugins";
    defConfig["resourcesDirectory"] = "../Resources";
#elif defined(__ANDROID__)
    defConfig["modulesDirectory"] = root + "/modules";
    defConfig["resourcesDirectory"] = root + "/res";
#else
    // Linux, BSD, etc.
    defConfig["modulesDirectory"] = INSTALL_PREFIX "/lib/sdriak/plugins";
    defConfig["resourcesDirectory"] = INSTALL_PREFIX "/share/sdriak";
#endif

    // Load config
    flog::info("Loading config");
    core::configManager.setPath(root + "/config.json");
    bool firstStart = !std::filesystem::exists(root + "/config.json");
    core::configManager.load(defConfig);

    // Before any module or network work: every libcurl handle reads it.
    {
        auto configAccess = core::configManager.read();
        auto network = configAccess.section("network");
        net::proxy::Config proxy;
        proxy.mode = net::proxy::modeFromName(network.value("proxyMode", std::string("system")));
        proxy.url = network.value("proxyUrl", std::string());
        proxy.user = network.value("proxyUser", std::string());
        proxy.password = network.value("proxyPassword", std::string());
        proxy.noProxy = network.value("noProxy", std::string());
        net::proxy::setConfig(proxy);
        flog::info("Network: proxy {}", net::proxy::effective().description);
    }

    core::configManager.enableAutoSave();

    float uiScaleFactor = 1.0f;
    {
        auto configAccess = core::configManager.edit();

    // Android can't load just any .so file. This means we have to hardcode the name of the modules
#ifdef __ANDROID__
        configAccess.set("modules", json::array({
            "airspy_source.so",
            "airspyhf_source.so",
            "hackrf_source.so",
            "hermes_source.so",
            "hydrasdr_source.so",
            "plutosdr_source.so",
            "qmx_source.so",
            "qmxserver_source.so",
            "rfspace_source.so",
            "rtl_sdr_source.so",
            "rtl_tcp_source.so",
            "sdriak_server_source.so",
            "spyserver_source.so",
            "kiwisdr_source.so",
            "ubersdr_source.so",
            "openwebrx_source.so",

            "network_sink.so",
            "audio_sink.so",

            "m17_decoder.so",
            "meteor_demodulator.so",
            "radio.so",
            "radiosonde_decoder.so",

            "frequency_manager.so",
            "noise_reduction_logmmse.so",
            "recorder.so",
            "rigctl_server.so",
            "scanner.so",
            "spots.so",
            "websdr_view.so",
        }));
#endif

        int storedConfigVersion = 0;
        const bool validConfigVersion =
            configAccess.tryGet("configVersion", storedConfigVersion);
        if (!validConfigVersion || storedConfigVersion < CURRENT_CONFIG_VERSION) {
            flog::info("Migrating core config schema from version {0} to {1}",
                       storedConfigVersion,
                       CURRENT_CONFIG_VERSION);
            // Version 1 establishes the explicit schema marker. Future migrations
            // run immediately before advancing this value.
            configAccess.set("configVersion", CURRENT_CONFIG_VERSION);
        }
        else if (storedConfigVersion > CURRENT_CONFIG_VERSION) {
            flog::warn("Core config schema version {0} is newer than supported version {1}; "
                       "unknown keys will be preserved",
                       storedConfigVersion, CURRENT_CONFIG_VERSION);
        }

        // Fix missing elements in config
        for (auto const& item : defConfig.items()) {
            const bool missing = !configAccess.contains(item.key());
            if (!configAccess.ensure(item.key(), item.value())) { continue; }
            if (missing) {
                flog::info("Missing key in config {0}, repairing", item.key());
            }
        }

        // Subtrees evolve independently of the flat core settings. Seed their
        // schema marker explicitly because the top-level repair above does not
        // recurse into an already existing object.
        ConfigManager::EditSection frequencyMemory = freq_memory::root(configAccess);
        int frequencyMemoryVersion = 0;
        if (!frequencyMemory.tryGet(freq_memory::VERSION, frequencyMemoryVersion) ||
            frequencyMemoryVersion < freq_memory::CURRENT_VERSION) {
            frequencyMemory.set(freq_memory::VERSION, freq_memory::CURRENT_VERSION);
        }
        else if (frequencyMemoryVersion > freq_memory::CURRENT_VERSION) {
            flog::warn("Frequency-memory config schema version {0} is newer than "
                       "supported version {1}; unknown keys will be preserved",
                       frequencyMemoryVersion, freq_memory::CURRENT_VERSION);
        }

        // Preserve unknown keys. They may belong to a newer application version;
        // deleting them here would make a temporary downgrade destructive.

        // Update to new module representation in config if needed
        ConfigManager::EditSection instances = configAccess.section("moduleInstances");
        std::vector<std::pair<std::string, std::string>> legacyInstances;
        if (const json* insts = instances.peek()) {
            for (auto const& [_name, inst] : insts->items()) {
                if (inst.is_string()) { legacyInstances.emplace_back(_name, inst.get<std::string>()); }
            }
        }
        for (auto const& [_name, mod] : legacyInstances) {
            json newMod;
            newMod["module"] = mod;
            newMod["enabled"] = true;
            instances.set(_name, newMod);
        }

        // Load UI scale factor; detected scale is not known yet (backend not initialized),
        // so set a temporary scale using the factor alone. The correct effective scale is
        // applied after backend::init() below, before any font loading.
        uiScaleFactor = configAccess.value("uiScaleFactor", 1.0f);
        style::setUIScale(uiScaleFactor);

        // Must be set before thememenu::init() applies the first scaled style.
        // The fallback keeps the per-platform default for configs predating the key.
        style::touchStyle = configAccess.value("touchStyle", style::touchStyle);

        style::migrateLogicalDimension(configAccess, "menuWidth", "menuWidthLogical", 250.0f, [](float value) {
            return style::uiScale > 1.0f && value > 300.0f;
        });
        style::migrateLogicalDimension(configAccess, "fftHeight", "fftHeightLogical", 150.0f, [](float value) {
            return style::uiScale > 1.0f && value >= 300.0f * style::uiScale;
        });
    }

    if (serverMode) {
        int rc = server::main();
        curl::cleanup();
        return rc;
    }

    std::string resDir = core::getResourcesDirectory();
    json bandColors = core::configManager.read().value("bandColors", json::object());

    // Check that the resource directory exists
    if (!std::filesystem::is_directory(resDir)) {
        flog::error("Resource directory doesn't exist! Please make sure that you've configured it correctly in config.json (check readme for details)");
        return 1;
    }

    // Initialize backend
    int biRes = backend::init(resDir);
    if (biRes < 0) { return biRes; }

    // Apply the correct effective scale now that the backend (GLFW) can report the actual
    // OS display scale. detectedScale is kept in memory only — not persisted to config.
    {
        float detected  = backend::getContentScale();
        float effective = std::clamp(detected * uiScaleFactor, 1.0f, 4.0f);
        style::setUIScale(effective);
        flog::info("UI scale: detected={:.2f}, factor={:.2f}, effective={:.2f}", detected, uiScaleFactor, effective);
    }

    if (firstStart) {
        core::configManager.edit().set("menuWidth", 300);
    }

    // Initialize SmGui in normal mode
    SmGui::init(false);

    if (!style::loadFonts(resDir)) { return -1; }
    thememenu::init(resDir);
    LoadingScreen::init();

    LoadingScreen::show("Loading icons");
    flog::info("Loading icons");
    if (!icons::load(resDir)) { return -1; }

    LoadingScreen::show("Loading band plans");
    flog::info("Loading band plans");
    bandplan::loadFromDir(resDir + "/bandplans");

    LoadingScreen::show("Loading band plan colors");
    flog::info("Loading band plans color table");
    bandplan::loadColorTable(bandColors);

    gui::mainWindow.init();

    flog::info("Ready.");

    // Run render loop (TODO: CHECK RETURN VALUE)
    backend::renderLoop();

    bool coreConfigSaved = true;

    // On android, none of this shutdown should happen due to the way the UI works
#ifndef __ANDROID__
    // The render loop has ended but modules are still alive, so the selected
    // radio mode can still be sampled for the active band's stack.
    freq_memory::commitCurrent();

    // Module instances are deliberately NOT destroyed here. Bulk teardown runs
    // largely unaudited destructors, and the source menu's reselect fallback
    // turns any source that never unregisters into a use-after-free. The OS
    // reclaims the instances at process exit instead.
    //
    // The cost is that each module's _END_ below closes its ConfigManager while
    // that module's instances are still alive, and config access after such a
    // shutdown throws. Module config access is confined to the GUI thread,
    // which the render loop has already retired, so the exposure is bounded.
    //
    // FIXME: restore the loop only together with the quiescent shutdown phase
    // described in doc/bugs/desktop-module-teardown.md.
    // while (!core::moduleManager.instances.empty()) {
    //     core::moduleManager.deleteInstance(core::moduleManager.instances.begin()->first);
    // }

    // Shut down all modules
    for (auto& [name, mod] : core::moduleManager.modules) {
        mod.end();
    }

    // Terminate backend (TODO: CHECK RETURN VALUE)
    backend::end();

    sigpath::iqFrontEnd.stop();

    coreConfigSaved = core::configManager.shutdown();
#endif

    curl::cleanup();

    if (coreConfigSaved) {
        flog::info("Exiting successfully");
        return 0;
    }
    flog::error("Exiting after failing to save the core config");
    return -1;
}
