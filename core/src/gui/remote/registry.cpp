#include "registry.h"

#include <chrono>

#include <utils/flog.h>

namespace sigpath {
    remote::Registry remoteRegistry;
}

namespace remote {

    Registry::~Registry() {
        shutdown();
    }

    void Registry::registerHandler(const RemoteHandler& handler) {
        std::lock_guard lck(m_mtx);
        m_handlers[handler.kind] = handler;
    }

    void Registry::unregisterHandler(RemoteKind kind) {
        std::unique_lock lck(m_mtx);
        if (m_probeKind == kind) {
            m_cancel = true;
            joinProbe(lck);
        }
        m_handlers.erase(kind);
    }

    bool Registry::hasHandler(RemoteKind kind) const {
        std::lock_guard lck(m_mtx);
        return m_handlers.count(kind) != 0;
    }

    std::optional<RemoteHandler> Registry::handler(RemoteKind kind) const {
        std::lock_guard lck(m_mtx);
        auto it = m_handlers.find(kind);
        if (it == m_handlers.end()) { return std::nullopt; }
        return it->second;
    }

    std::string Registry::unavailableReason(const RemoteEntry& entry) const {
        std::lock_guard lck(m_mtx);
        auto it = m_handlers.find(entry.kind);
        if (it == m_handlers.end()) { return std::string("The ") + kindName(entry.kind) + " source is not loaded"; }
        if (entry.tlsRequired && !it->second.supportsTls) {
            return std::string("The ") + kindName(entry.kind) + " source has no TLS (https) support yet";
        }
        if (entry.hasFlag("audio-only")) { return "Server too old for IQ: needs OpenWebRX+ 1.2.129 or later"; }
        return {};
    }

    void Registry::joinProbe(std::unique_lock<std::mutex>& lck) {
        // The probe thread takes m_mtx to publish its result; wait for it to
        // finish without holding the lock, then join.
        m_cv.wait(lck, [this] { return !m_probeRunning; });
        if (m_probeThread.joinable()) {
            std::thread t = std::move(m_probeThread);
            lck.unlock();
            t.join();
            lck.lock();
        }
    }

    bool Registry::startProbe(const RemoteEntry& entry) {
        std::unique_lock lck(m_mtx);
        if (m_probeRunning) { return false; }
        auto it = m_handlers.find(entry.kind);
        if (it == m_handlers.end() || !it->second.probe) { return false; }
        if (m_probeThread.joinable()) {
            std::thread t = std::move(m_probeThread);
            lck.unlock();
            t.join();
            lck.lock();
            if (m_probeRunning) { return false; }
        }
        auto probe = it->second.probe;
        m_cancel = false;
        m_probeRunning = true;
        m_probeKind = entry.kind;
        m_state.running = true;
        m_state.entryId = entry.id;
        m_state.entryName = entry.name;
        m_state.result.reset();
        m_state.status = "Testing " + entry.target.hostPort() + " ...";
        m_probeThread = std::thread([this, probe, entry] {
            const auto t0 = std::chrono::steady_clock::now();
            ProbeResult result;
            try {
                result = probe(entry, m_cancel);
            }
            catch (const std::exception& e) {
                result.ok = false;
                result.reason = e.what();
            }
            const auto ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard l(m_mtx);
            if (m_cancel) {
                m_state.status = "Test cancelled";
            }
            else if (result.ok) {
                m_state.status = "OK: " + entry.target.hostPort() +
                                 (result.rttMs ? " (" + std::to_string(*result.rttMs) + " ms)" : " (" + std::to_string(ms) + " ms)");
            }
            else {
                m_state.status = "Not OK: " + entry.target.hostPort() + (result.reason.empty() ? std::string() : ": " + result.reason);
            }
            m_state.result = std::move(result);
            m_state.running = false;
            m_probeRunning = false;
            m_probeKind.reset();
            m_cv.notify_all();
        });
        return true;
    }

    void Registry::cancelProbe() {
        m_cancel = true;
    }

    ProbeState Registry::probeState() const {
        std::lock_guard lck(m_mtx);
        return m_state;
    }

    void Registry::shutdown() {
        std::unique_lock lck(m_mtx);
        m_cancel = true;
        joinProbe(lck);
    }

}
