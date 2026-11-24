#pragma once

// Core "Network" menu: the proxy every libcurl handle uses (utils/proxy.h),
// persisted in the "network" config section. core.cpp loads the setting at
// startup, so server mode uses it too.
namespace networkmenu {
    void init();
    void draw(void* ctx);
}
