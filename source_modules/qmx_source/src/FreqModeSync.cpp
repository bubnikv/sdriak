#include "FreqModeSync.h"

#include <core.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <module_com.h>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>

#include <cmath>
#include <string>
#include <utility>


// Frequency model

double FreqModeSync::qmxRigToIqOffset(const qmx::QmxStatus& status) {
    const double cwOffsetHz = status.hasCwOffset() ? static_cast<double>(status.cwOffsetHz) : kQmxDefaultCwOffsetHz;
    double offset = kQmxIfOffsetHz;
    if (!status.hasMode())
        return offset;
    switch (status.mode) {
    case qmx::QmxMode::CW:  return offset + cwOffsetHz;
    case qmx::QmxMode::CWR: return offset - cwOffsetHz;
    default:                return offset;
    }
}

double FreqModeSync::rigFrequencyToCenterFrequency(std::int64_t rigFreq, const qmx::QmxStatus& status) {
    return static_cast<double>(rigFreq) - qmxRigToIqOffset(status);
}

std::int64_t FreqModeSync::centerFrequencyToRigFrequency(double centerFreq, const qmx::QmxStatus& status) {
    return static_cast<std::int64_t>(std::llround(centerFreq + qmxRigToIqOffset(status)));
}

std::int64_t FreqModeSync::effectiveReceiveRigFrequency(const qmx::QmxStatus& status) {
    std::int64_t frequency = status.frequency;
    if (status.hasRit() && status.hasRitEnabled() && status.ritEnabled)
        frequency += status.ritHz;
    return frequency;
}

int FreqModeSync::qmxModeToRadioIface(qmx::QmxMode mode) {
    switch (mode) {
    case qmx::QmxMode::LSB: return RADIO_IFACE_MODE_LSB;
    case qmx::QmxMode::USB: return RADIO_IFACE_MODE_USB;
    case qmx::QmxMode::CW:  return RADIO_IFACE_MODE_CW;
    case qmx::QmxMode::CWR: return RADIO_IFACE_MODE_CWR;
    case qmx::QmxMode::AM:  return RADIO_IFACE_MODE_AM;
    default:                return -1;
    }
}

qmx::QmxMode FreqModeSync::radioIfaceToQmxMode(int radioMode) {
    switch (radioMode) {
    case RADIO_IFACE_MODE_LSB: return qmx::QmxMode::LSB;
    case RADIO_IFACE_MODE_USB: return qmx::QmxMode::USB;
    case RADIO_IFACE_MODE_CW:  return qmx::QmxMode::CW;
    case RADIO_IFACE_MODE_CWR: return qmx::QmxMode::CWR;
    case RADIO_IFACE_MODE_AM:  return qmx::QmxMode::AM;
    default:                   return qmx::QmxMode::UNKNOWN;
    }
}

void FreqModeSync::setDevice(qmx::QmxDevice* device) {
    m_device = device;
}

void FreqModeSync::start(bool syncVfo) {
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_pendingStatus = {};
        m_hasPendingStatus = false;
    }
    m_status = {};
    m_iqCenterFreq = -1.;
    m_syncVfo = syncVfo;
    m_running = true;
    if (syncVfo)
        // Temporary best effort value before the update is received from QMX.
        tuner::lockVFOtoCenter(FreqModeSync::kQmxIfOffsetHz);
}

void FreqModeSync::stop() {
    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_pendingStatus = {};
        m_hasPendingStatus = false;
    }
    m_status = {};
    tuner::unlockVFO();
}

void FreqModeSync::setSyncVfo(bool enabled) { 
    m_syncVfo = enabled;
    if (enabled) {
        if (m_status.hasMode())
            tuner::lockVFOtoCenter(qmxRigToIqOffset(m_status));
    } else {
        tuner::unlockVFO();
    }
}

// Called by SDRIAK when the IQ center frequency changes (tune callback, GUI thread).
// This is the SDRIAK -> QMX direction. We:
//   1. Compute the new rig frequency from the cached status.
//   2. Send that rig frequency to QMX.
//   3. Update the cached status so it reflects the new rig frequency immediately
//      (as if QMX had already confirmed it).
//   4. If syncVfo: place the SDRIAK VFO at the new rig frequency.
// This way the cached status is always the source of truth and tick() won't
// produce a feedback bounce.
void FreqModeSync::onIqCenterChanged(double newFreq)
{
    if (m_iqCenterFreq == newFreq)
        return;

//    flog::debug("FreqModeSync::onIqCenterChanged: {}", newFreq);

    if (!m_running || !m_status.hasFrequency() || (m_status.hasTransmit() && m_status.transmit))
        return;

    m_iqCenterFreq = newFreq;

    // 1. Compute desired rig frequency.
    std::int64_t newRigFreq = centerFrequencyToRigFrequency(newFreq, m_status);
    std::int64_t curRigFreq = effectiveReceiveRigFrequency(m_status);
    if (newRigFreq == curRigFreq)
        return;

    // 2. Send to QMX (enqueued, non-blocking).
    //    Use the active receive VFO (A/B) from cached status to send FA or FB.
    int rxVfo = m_status.hasRxVfo() ? m_status.rxVfo : 0;
    std::string error;
    if (!m_device->setFrequency(newRigFreq, rxVfo, &error)) {
        flog::warn("FreqModeSync: {}", error);
        return;
    }
//    flog::debug("FreqModeSync::onIqCenterChanged: QMX frequency updated to {}", newRigFreq);

    // 3. Update cached status immediately.
    m_status.frequency = newRigFreq;
    m_status.setFlag(qmx::QmxStatusFlag::Frequency);
    assert(m_status.hasFrequency());
    // Also suppress a stale pending status from overwriting this.
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_pendingStatus.clearFlag(qmx::QmxStatusFlag::Frequency);
    }
#ifndef NDEBUG
    {
        const std::int64_t rigFreq = effectiveReceiveRigFrequency(m_status);
        const double centerFrequency = rigFrequencyToCenterFrequency(rigFreq, m_status);
        assert(std::llround(m_iqCenterFreq) == std::llround(centerFrequency));
    }
#endif // NDEBUG
}

void FreqModeSync::onStatusReceived(const qmx::QmxStatus& status) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_pendingStatus = status;
    m_hasPendingStatus = true;
//    flog::debug("FreqModeSync::onStatusReceived: QMX frequency updated to {}", status.hasFrequency() ? status.frequency : -1);
}

// Per-frame tick
void FreqModeSync::tick()
{
    if (!m_running)
        return;

    if (gui::mainWindow.getTuningMode() == tuner::TUNER_MODE_NORMAL) {
        // Suppress tuning by dragging if the VFO is synchronized.
        gui::waterfall.VFOMoveSingleClick = m_syncVfo;
    }

    qmx::QmxStatusFlags qmxStatusUpdate { 0 };
    qmx::QmxStatusFlags qmxStatusDelivered { 0 };
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        if (m_hasPendingStatus) {
            qmxStatusDelivered = m_pendingStatus.flags;
            if (qmxStatusDelivered)
                qmxStatusUpdate = m_status.updated_with(m_pendingStatus);
            if (qmxStatusUpdate)
                m_status += m_pendingStatus;
            m_hasPendingStatus = false;
        }
    }

    if ((qmxStatusUpdate & static_cast<qmx::QmxStatusFlags>(qmx::QmxStatusFlag::Frequency)) != 0 && 
        !(m_status.hasTransmit() && m_status.transmit)) {
        const std::int64_t rigFreq = effectiveReceiveRigFrequency(m_status);
        const double centerFrequency = rigFrequencyToCenterFrequency(rigFreq, m_status);
        //flog::debug("FreqModeSync::tick(): QMX frequency updated to {}", m_status.hasFrequency() ? m_status.frequency : -1);
        // Update SDRIAK IQ center if it doesn't match the cached rig frequency.
        // tuner::tune(IQ_ONLY) calls our onIqCenterChanged, which will recompute
        // the same rig frequency from the just-updated cache -> no-op, no feedback.
        if (std::llround(m_iqCenterFreq) != std::llround(centerFrequency)) {
//                flog::debug("FreqModeSync::tick(): QMX frequency {}, old centerFreuqency {} new centerFrequency {}", rigFreq, m_iqCenterFreq, centerFrequency);
            bool tuned = false;
            if (m_syncVfo) {
                std::string vfoName = gui::waterfall.selectedVFO;
                if (!vfoName.empty() && sigpath::vfoManager.vfoExists(vfoName)) {
                    double vfoAbsFreq = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(vfoName);
                    if (std::llround(vfoAbsFreq) != rigFreq) {
                        tuner::lockVFOtoCenter(qmxRigToIqOffset(m_status));
                        // tuner::TUNE_MODE_NORMAL and TUNE_MODE_CENTER do the same if tuner::lockVFOtoCenter() is active
                        tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, static_cast<double>(rigFreq));
                        tuned = true;
                    }
                }
            }
            if (! tuned)
                tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", centerFrequency);
        }
    }

    const std::string &vfoName = gui::waterfall.selectedVFO;
    if (m_syncVfo && m_status.hasMode() && ! vfoName.empty() &&
        sigpath::vfoManager.vfoExists(vfoName) && core::modComManager.getModuleName(vfoName) == "radio") {
        if (int targetMode = qmxModeToRadioIface(m_status.mode); targetMode >= 0) {
            int currentRadioMode = -1;
            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_MODE, NULL, &currentRadioMode);
            if (currentRadioMode >= 0 && currentRadioMode != targetMode) {
                // The QMX and SDRIAK current VFO modes are out of sync.
                if ((qmxStatusUpdate & static_cast<qmx::QmxStatusFlags>(qmx::QmxStatusFlag::Mode)) != 0) {
                    // QMX provided an update of mode field. Synchronizing SDRIAK VFO mode to QMX VFO mode.
                    tuner::lockVFOtoCenter(qmxRigToIqOffset(m_status));
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &targetMode, NULL);
                    // Retune if the center frequency changed.
//                    if (tuner::lockVFOtoCenter(qmxRigToIqOffset(m_status)) && m_status.hasFrequency())
//                        tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, static_cast<double>(effectiveReceiveRigFrequency(m_status)));
                } else {
                    // Update QMX mode from the changed radio mode.
                    // Check whether the user did not try to switch to the opposite CW mode (CW vs CWR).
                    bool wrongMode = false;
                    if (currentRadioMode == RADIO_IFACE_MODE_CW || currentRadioMode == RADIO_IFACE_MODE_CWR) {
                        if (m_status.hasCwOffset()) {
                            targetMode = m_status.cwOffsetHz > 0 ? RADIO_IFACE_MODE_CW : RADIO_IFACE_MODE_CWR;
                            wrongMode = currentRadioMode != targetMode;
                            if (wrongMode)
                                core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &targetMode, NULL);
                        } else {
                            wrongMode = true;
                        }
                    }
                    // Update cached status immediately.
                    if (! wrongMode) {
                        m_status.mode = radioIfaceToQmxMode(currentRadioMode);
                        m_status.setFlag(qmx::QmxStatusFlag::Mode);
                        assert(m_status.hasMode());
                        std::string error;
                        if (!m_device->setMode(m_status.mode, &error)) {
                            flog::warn("FreqModeSync: {}", error);
                            // The current SDRIAK mode is not supported by QMX. Revert the SDRIAK mode to the QMX mode.
                            core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &currentRadioMode, NULL);
                            return;
                        }
                        // Also suppress a stale pending status from overwriting this.
                        {
                            std::lock_guard<std::mutex> lock(m_statusMutex);
                            m_pendingStatus.clearFlag(qmx::QmxStatusFlag::Mode);
                        }
                        if (tuner::lockVFOtoCenter(qmxRigToIqOffset(m_status)) && m_status.hasFrequency()) {
                            // SDRIAK mode change triggered change of a VFO to IQ center frequency.
                            // Retune.
                            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, static_cast<double>(effectiveReceiveRigFrequency(m_status)));
                        }
                    }
                }
            }
        }
    }
}
