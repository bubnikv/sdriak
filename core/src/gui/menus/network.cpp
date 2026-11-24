#include <gui/menus/network.h>

#include <chrono>
#include <cstring>
#include <string>

#include <core.h>
#include <gui/style.h>
#include <imgui.h>
#include <utils/proxy.h>

namespace networkmenu {
    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr const char* kModes = "None\0System\0Manual\0";
        constexpr int kModeIds[] = { (int)net::proxy::Mode::None, (int)net::proxy::Mode::System, (int)net::proxy::Mode::Manual };

        int modeId = 1;
        char url[512] = "";
        char user[256] = "";
        char password[256] = "";
        char noProxy[1024] = "";

        // The resolved setting reads the Windows registry; refresh it about
        // once a second instead of every frame.
        std::string inEffect;
        Clock::time_point inEffectAt;

        void copyTo(char* dst, size_t size, const std::string& src) {
            std::strncpy(dst, src.c_str(), size - 1);
            dst[size - 1] = 0;
        }

        net::proxy::Config fromFields() {
            net::proxy::Config c;
            c.mode = (net::proxy::Mode)kModeIds[modeId];
            c.url = url;
            c.user = user;
            c.password = password;
            c.noProxy = noProxy;
            return c;
        }

        void refreshInEffect() {
            inEffect = net::proxy::effective().description;
            inEffectAt = Clock::now();
        }

        void apply() {
            const net::proxy::Config c = fromFields();
            net::proxy::setConfig(c);
            {
                auto configAccess = core::configManager.edit();
                auto network = configAccess.section("network");
                network.set("proxyMode", std::string(net::proxy::modeName(c.mode)));
                network.set("proxyUrl", c.url);
                network.set("proxyUser", c.user);
                network.set("proxyPassword", c.password);
                network.set("noProxy", c.noProxy);
            }
            refreshInEffect();
        }

        // A text field saved when editing ends, not on every keystroke.
        void field(const char* label, const char* id, const char* hint, char* buf, size_t size,
                   ImGuiInputTextFlags flags = 0) {
            ImGui::LeftLabelFill(label);
            ImGui::InputTextWithHint(id, hint, buf, size, flags);
            if (ImGui::IsItemDeactivatedAfterEdit()) { apply(); }
        }
    }

    void init() {
        const net::proxy::Config c = net::proxy::config();
        for (int i = 0; i < 3; i++) {
            if (kModeIds[i] == (int)c.mode) { modeId = i; }
        }
        copyTo(url, sizeof(url), c.url);
        copyTo(user, sizeof(user), c.user);
        copyTo(password, sizeof(password), c.password);
        copyTo(noProxy, sizeof(noProxy), c.noProxy);
        refreshInEffect();
    }

    void draw(void* ctx) {
        ImGui::LeftLabelFill("Proxy");
        if (ImGui::Combo("##sdrpp_net_proxy_mode", &modeId, kModes)) { apply(); }

        if (kModeIds[modeId] == (int)net::proxy::Mode::Manual) {
            field("URL", "##sdrpp_net_proxy_url", "http://host:port", url, sizeof(url));
            field("User", "##sdrpp_net_proxy_user", "optional", user, sizeof(user));
            field("Password", "##sdrpp_net_proxy_password", "optional", password, sizeof(password),
                  ImGuiInputTextFlags_Password);
            field("No proxy", "##sdrpp_net_no_proxy", "localhost,.lan,10.0.0.0/8", noProxy, sizeof(noProxy));
            if (password[0]) {
                ImGui::TextDisabled("The password is stored in plain text in config.json.");
            }
        }

        if (Clock::now() - inEffectAt > std::chrono::seconds(1)) { refreshInEffect(); }
        ImGui::TextWrapped("In effect: %s", inEffect.c_str());
    }
}
