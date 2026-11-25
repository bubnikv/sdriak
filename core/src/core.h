#pragma once
#include <config.h>
#include <module.h>
#include <module_com.h>
#include <string>
#include "command_args.h"

namespace core {
    SDRPP_EXPORT ConfigManager configManager;
    SDRPP_EXPORT ModuleManager moduleManager;
    SDRPP_EXPORT ModuleComManager modComManager;
    SDRPP_EXPORT CommandArgsParser args;

    void setInputSampleRate(double samplerate);
    // The waterfall span a source's server shows, independent of the IQ
    // rate: resets zoom, clears the waterfall history on a change and keeps
    // the VFOs inside. clearDisplaySpan() returns to the effective sample
    // rate. UI thread; both are no-ops in --server mode.
    void setDisplaySpan(double spanHz);
    void clearDisplaySpan();

    // Commit live UI state that is not continuously persisted, then synchronously
    // flush every autosaved core and module config. This is non-terminal because
    // Android may resume after the lifecycle event.
    SDRPP_CPP_EXPORT bool saveState();

    // Effective resource paths. Returns the value from the config,
    // except under AppImage builds (BUILD_APPIMAGE) on Linux when $APPDIR
    // is set — in which case the bundled paths inside the AppImage mount
    // are returned. The accessor pattern keeps the FUSE mount point out
    // of the persisted config file. Both take the config lock themselves, so
    // they must not be called while this manager has an active access.
    SDRPP_CPP_EXPORT std::string getModulesDirectory();
    SDRPP_CPP_EXPORT std::string getResourcesDirectory();

    // Resolve a possibly relative path from the config against the
    // executable's directory (see core.cpp for rationale).
    SDRPP_CPP_EXPORT std::string resolveConfigPath(const std::string& path);
};

int sdrpp_main(int argc, char* argv[]);
