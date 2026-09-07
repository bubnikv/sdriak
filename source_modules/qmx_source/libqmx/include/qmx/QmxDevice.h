#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <qmx/QmxCatDebug.h>

namespace qmx {
    namespace detail {
        class DeviceImpl;
    }

    constexpr int kSampleRate = 48000;
    // Serial baud rate is a placeholder with no real meaning for QMX CDC.
    constexpr int kSerialBaudRate = 115200;
    // Audio input buffer for one second of audio at 48 kHz, stereo, 24 bit signed int samples,
    // aligned to the QMX USB audio endpoint packet size.
    constexpr std::size_t kStreamBlockSize = 512;

    struct AudioDeviceInfo {
        std::string id;
        std::string label;
    };

    struct SerialPortInfo {
        std::string path;
        std::string label;
    };

    struct AndroidUsbDeviceInfo {
        int fd = -1;

        bool valid() const { return fd >= 0; }
    };

    struct StartOptions {
        std::string audioDeviceId;
        std::string serialPort;
        AndroidUsbDeviceInfo androidUsb;
        bool enableIqMode = true;
    };

    struct IQSample {
        float i = 0.0f;
        float q = 0.0f;
    };

    enum class QmxMode {
        UNKNOWN,
        LSB,
        USB,
        CW,
        FM, // Parsed for Kenwood MD compatibility; QMX does not use this yet.
        AM,
        CWR,
        FSK, // aka DIGI
        FSKR, // aka DIGI reversed
    };

    using QmxStatusFlags = std::uint32_t;

    enum class QmxStatusFlag : QmxStatusFlags {
        None               = 0,
        Frequency     = 1u << 0,
        VfoAFrequency = 1u << 1,
        VfoBFrequency = 1u << 2,
        Transmit      = 1u << 3,
        Mode          = 1u << 4,
        RxVfo         = 1u << 5,
        TxVfo         = 1u << 6,
        Split         = 1u << 7,
        Rit           = 1u << 8,
        RitEnabled    = 1u << 9,
        SMeter        = 1u << 10,
        Power         = 1u << 11,
        SWR           = 1u << 12,
        CwOffset      = 1u << 13,
    };

    constexpr QmxStatusFlags qmxStatusFlagMask(QmxStatusFlag flag) {
        return static_cast<QmxStatusFlags>(flag);
    }

    constexpr QmxStatusFlags operator|(QmxStatusFlag lhs, QmxStatusFlag rhs) {
        return qmxStatusFlagMask(lhs) | qmxStatusFlagMask(rhs);
    }

    constexpr QmxStatusFlags operator|(QmxStatusFlags lhs, QmxStatusFlag rhs) {
        return lhs | qmxStatusFlagMask(rhs);
    }

    // Queried regularly from QMX over CAT (USB CDC connection).
    struct QmxStatus {
        QmxStatusFlags flags = 0;
        std::int64_t frequency = 0;
        std::int64_t vfoAFrequency = 0;
        std::int64_t vfoBFrequency = 0;
        bool transmit = false;
        QmxMode mode = QmxMode::UNKNOWN;
        int rxVfo = -1;
        int txVfo = -1;
        bool split = false;
        int ritHz = 0;
        bool ritEnabled = false;
        int sMeterDb = 0;
        int powerTenthsW = 0;
        int swrHundredths = 0;
        int cwOffsetHz = 0;
        std::uint64_t sequence = 0;
#if QMX_CAT_DEBUG_TIMING
        QmxCatTimingDebug catDebug;
#endif

        bool hasFlag(QmxStatusFlag flag) const {
            return (flags & qmxStatusFlagMask(flag)) != 0;
        }

        bool hasAll(QmxStatusFlags mask) const {
            return (flags & mask) == mask;
        }

        void setFlag(QmxStatusFlag flag, bool enabled = true) {
            if (enabled) {
                flags |= qmxStatusFlagMask(flag);
            } else {
                flags &= ~qmxStatusFlagMask(flag);
            }
        }

        void clearFlags(QmxStatusFlags mask) {
            flags &= ~mask;
        }

        void clearFlag(QmxStatusFlag flag) {
            clearFlags(qmxStatusFlagMask(flag));
        }

        bool updated_with(const QmxStatus& incoming) {
            return (incoming.hasFrequency() && (!this->hasFrequency() || frequency != incoming.frequency)) ||
                   (incoming.hasVfoAFrequency() && (!this->hasVfoAFrequency() || vfoAFrequency != incoming.vfoAFrequency)) ||
                   (incoming.hasVfoBFrequency() && (!this->hasVfoBFrequency() || vfoBFrequency != incoming.vfoBFrequency)) ||
                   (incoming.hasTransmit() && (!this->hasTransmit() || transmit != incoming.transmit)) ||
                   (incoming.hasMode() && (!this->hasMode() || mode != incoming.mode)) ||
                   (incoming.hasRxVfo() && (!this->hasRxVfo() || rxVfo != incoming.rxVfo)) ||
                   (incoming.hasTxVfo() && (!this->hasTxVfo() || txVfo != incoming.txVfo)) ||
                   (incoming.hasSplit() && (!this->hasSplit() || split != incoming.split)) ||
                   (incoming.hasRit() && (!this->hasRit() || ritHz != incoming.ritHz)) ||
                   (incoming.hasRitEnabled() && (!this->hasRitEnabled() || ritEnabled != incoming.ritEnabled)) ||
                   (incoming.hasSMeter() && (!this->hasSMeter() || sMeterDb != incoming.sMeterDb)) ||
                   (incoming.hasPower() && (!this->hasPower() || powerTenthsW != incoming.powerTenthsW)) ||
                   (incoming.hasSWR() && (!this->hasSWR() || swrHundredths != incoming.swrHundredths)) ||
                   (incoming.hasCwOffset() && (!this->hasCwOffset() || cwOffsetHz != incoming.cwOffsetHz));
        }

        QmxStatus& operator+=(const QmxStatus& incoming) {
            flags |= incoming.flags;
            sequence = incoming.sequence;
#if QMX_CAT_DEBUG_TIMING
            catDebug = incoming.catDebug;
#endif

            if (incoming.hasFrequency())
                frequency = incoming.frequency;
            if (incoming.hasVfoAFrequency())
                vfoAFrequency = incoming.vfoAFrequency;
            if (incoming.hasVfoBFrequency())
                vfoBFrequency = incoming.vfoBFrequency;
            if (incoming.hasTransmit())
                transmit = incoming.transmit;
            if (incoming.hasMode())
                mode = incoming.mode;
            if (incoming.hasRxVfo())
                rxVfo = incoming.rxVfo;
            if (incoming.hasTxVfo())
                txVfo = incoming.txVfo;
            if (incoming.hasSplit())
                split = incoming.split;
            if (incoming.hasRit())
                ritHz = incoming.ritHz;
            if (incoming.hasRitEnabled())
                ritEnabled = incoming.ritEnabled;
            if (incoming.hasSMeter())
                sMeterDb = incoming.sMeterDb;
            if (incoming.hasPower())
                powerTenthsW = incoming.powerTenthsW;
            if (incoming.hasSWR())
                swrHundredths = incoming.swrHundredths;
            if (incoming.hasCwOffset())
                cwOffsetHz = incoming.cwOffsetHz;

            return *this;
        }

        bool hasFrequency() const { return hasFlag(QmxStatusFlag::Frequency); }
        bool hasVfoAFrequency() const { return hasFlag(QmxStatusFlag::VfoAFrequency); }
        bool hasVfoBFrequency() const { return hasFlag(QmxStatusFlag::VfoBFrequency); }
        bool hasTransmit() const { return hasFlag(QmxStatusFlag::Transmit); }
        bool hasMode() const { return hasFlag(QmxStatusFlag::Mode); }
        bool hasRxVfo() const { return hasFlag(QmxStatusFlag::RxVfo); }
        bool hasTxVfo() const { return hasFlag(QmxStatusFlag::TxVfo); }
        bool hasSplit() const { return hasFlag(QmxStatusFlag::Split); }
        bool hasRit() const { return hasFlag(QmxStatusFlag::Rit); }
        bool hasRitEnabled() const { return hasFlag(QmxStatusFlag::RitEnabled); }
        bool hasSMeter() const { return hasFlag(QmxStatusFlag::SMeter); }
        bool hasPower() const { return hasFlag(QmxStatusFlag::Power); }
        bool hasSWR() const { return hasFlag(QmxStatusFlag::SWR); }
        bool hasCwOffset() const { return hasFlag(QmxStatusFlag::CwOffset); }
    };

    // Called by QMX driver when a new IQ sample block is available.
    using StreamCallback = void (*)(const IQSample* samples, std::size_t count, void* ctx);
    // Called by QMX driver when a new status update is available from CAT.
    using StatusCallback = void (*)(const QmxStatus& status, void* ctx);
#if QMX_CAT_RAW_LOG
    using CatLogCallback = void (*)(const QmxCatLogEntry& entry, void* ctx);
#endif

    class QmxDevice {
    public:
        QmxDevice();
        ~QmxDevice();

        QmxDevice(const QmxDevice&) = delete;
        QmxDevice& operator=(const QmxDevice&) = delete;

        static bool isSupported();
        static std::vector<AudioDeviceInfo> listAudioDevices();
        static std::vector<SerialPortInfo> listSerialPorts();

        bool start(const StartOptions& options,
                   StreamCallback callback,
                   void* ctx,
                   StatusCallback statusCallback = nullptr,
                   void* statusCtx = nullptr,
#if QMX_CAT_RAW_LOG
                   CatLogCallback catLogCallback = nullptr,
                   void* catLogCtx = nullptr,
#endif
                   std::string* error = nullptr);
        void stop();

        bool isStreaming() const;
        // Set QMX radio frequency by sending "FA" or "FB" Kenwood 480 compatible CAT command over USB CDC,
        // matching the active receive VFO (0 = VFO A → FA, 1 = VFO B → FB).
        // The frequency is the QMX radio dial frequency, accounting for mode-specific offsets like CW pitch.
        bool setFrequency(std::int64_t hz, int vfo = 0, std::string* error = nullptr);
        // Set QMX radio mode by sending "MD" Kenwood 480 compatible CAT command over USB CDC.
        bool setMode(QmxMode mode, std::string* error = nullptr);

        std::string lastError() const;

    private:
        void setError(const std::string& error, std::string* out);

        std::unique_ptr<detail::DeviceImpl> impl;
        std::string error;
    };
}