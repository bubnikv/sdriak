#pragma once

#include <qmx/QmxDevice.h>

#include <cstdint>
#include <mutex>
#include <string>

// Encapsulates bidirectional frequency/mode synchronization between QMX and SDRIAK.
//
// The cached QmxStatus is the single source of truth for frequency.
//
// SDRIAK → QMX direction:
//   onIqCenterChanged() converts the new IQ center to rig frequency using the
//   cached status, sends it to QMX, and immediately updates the cached status
//   so that tick() sees no difference and produces no feedback.
//
// QMX → SDRIAK direction:
//   tick() applies any pending polled status.  If the cached rig frequency
//   differs from the current SDRIAK IQ center, it calls tuner::tune which
//   triggers onIqCenterChanged — but that recomputes the same rig frequency
//   from the just-cached value, so it's a no-op.
//
// Threading contract:
//   - onStatusReceived() is called from the CatPoller thread.
//   - All other methods are called from the GUI thread only.
class FreqModeSync {
public:
    static constexpr double kQmxIfOffsetHz = 12000.0;
    static constexpr double kQmxDefaultCwOffsetHz = 700.0;

    // ── Frequency model (pure, stateless helpers) ──────────────────────

    static double       qmxRigToIqOffset(const qmx::QmxStatus& status);
    static double       rigFrequencyToCenterFrequency(std::int64_t rigFreq, const qmx::QmxStatus& status);
    static std::int64_t centerFrequencyToRigFrequency(double centerFreq, const qmx::QmxStatus& status);
    static std::int64_t effectiveReceiveRigFrequency(const qmx::QmxStatus& status);

    static int          qmxModeToRadioIface(qmx::QmxMode mode);
    static qmx::QmxMode radioIfaceToQmxMode(int radioMode);

    // ── Lifecycle ──────────────────────────────────────────────────────

    // Bind to the device that will receive setFrequency/setMode commands.
    void setDevice(qmx::QmxDevice* device);

    // Reset all sync state.  Called when streaming starts.
    void start(bool syncVfo);

    // Reset all sync state.  Called when streaming stops.
    void stop();

    // ── Events (inputs) ───────────────────────────────────────────────

    // SDRIAK changed the IQ center frequency (tune callback).
    void onIqCenterChanged(double newFreq);

    // CatPoller delivered a new status snapshot (called from poller thread).
    void onStatusReceived(const qmx::QmxStatus& status);

    // ── Per-frame tick (GUI thread) ───────────────────────────────────

    // Run one iteration of bidirectional sync.  Call every GUI frame.
    void tick();

    // ── Configuration ─────────────────────────────────────────────────

    void setSyncVfo(bool enabled);
    bool getSyncVfo() const { return m_syncVfo; }

    // ── Accessors for UI display ──────────────────────────────────────

    double getFreq() const { return m_iqCenterFreq; }
    const qmx::QmxStatus& currentStatus() const { return m_status; }

private:
    qmx::QmxDevice* m_device = nullptr;
    bool m_running = false;
    double m_iqCenterFreq = -1.;
    bool m_syncVfo = false;

    // Pending status delivered by poller thread, consumed by tick() on GUI thread.
    std::mutex m_statusMutex;
    qmx::QmxStatus m_pendingStatus;
    bool m_hasPendingStatus = false;

    // Cached QMX status — the single source of truth, only touched on GUI thread.
    qmx::QmxStatus m_status;
};
