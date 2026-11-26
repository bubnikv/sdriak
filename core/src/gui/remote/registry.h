#pragma once
// What each networked source module registers with the remote-receiver
// browser: a prober and a connect handler. Core never depends on a module.
// Plan: doc/todo/remote-receiver-browser.md.

#include "remote_entry.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <module.h>

namespace remote {

    struct ProbeResult {
        bool ok = false;
        std::string reason;       // why not: "too_busy", "badp", "TLS required", "refused", ...
        std::optional<int> rttMs; // first answer
        std::vector<std::pair<std::string, std::string>> negotiated; // rates, modes, control
    };

    struct RemoteHandler {
        RemoteKind kind = RemoteKind::Other;
        std::string sourceName;  // as registered with SourceManager
        bool supportsTls = true; // false: entries needing TLS are greyed
        // Worker thread. Short, closes what it opens; polls cancel.
        std::function<ProbeResult(const RemoteEntry&, const std::atomic<bool>& cancel)> probe;
        // UI thread, after the browser selected sourceName. Fills the
        // module's host/port or URL and connects as its own button would.
        std::function<void(const RemoteEntry&)> connect;
        // UI thread. A transient reason to defer Connect, if any.
        std::function<std::string()> connectBlockedReason;
    };

    // One probe at a time, across every browser view.
    struct ProbeState {
        bool running = false;
        std::string entryId;   // the entry probed last
        std::string entryName;
        std::optional<ProbeResult> result; // of the last finished probe
        std::string status;    // one line for the UI
    };

    class Registry {
    public:
        Registry() = default;
        ~Registry();

        // Module constructor / destructor. Unregistering cancels and waits
        // for a probe of that kind, so module code never runs after its
        // module is gone.
        void registerHandler(const RemoteHandler& handler);
        void unregisterHandler(RemoteKind kind);

        bool hasHandler(RemoteKind kind) const;
        std::optional<RemoteHandler> handler(RemoteKind kind) const;
        // Why an entry cannot be connected to, empty when it can.
        std::string unavailableReason(const RemoteEntry& entry) const;
        uint64_t handlerGeneration() const { return m_handlerGeneration.load(); }
        std::map<RemoteKind, bool> tlsSupportSnapshot(uint64_t& generation) const;

        // Start a probe; false when one is running or the kind has no prober.
        bool startProbe(const RemoteEntry& entry);
        void cancelProbe();
        ProbeState probeState() const;
        // Stop and join the probe thread (core shutdown).
        void shutdown();

    private:
        void joinProbe(std::unique_lock<std::mutex>& lck);

        mutable std::mutex m_mtx;
        std::condition_variable m_cv;
        std::map<RemoteKind, RemoteHandler> m_handlers;
        std::atomic<uint64_t> m_handlerGeneration{0};
        std::thread m_probeThread;
        std::atomic<bool> m_cancel{ false };
        bool m_probeRunning = false;
        std::optional<RemoteKind> m_probeKind;
        ProbeState m_state;
    };

}

namespace sigpath {
    SDRPP_EXPORT remote::Registry remoteRegistry;
}
