#include "QmxCatStatus.h"

#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {
    bool parseUnsigned(std::string_view text, std::int64_t& value) {
        if (text.empty())
            return false;

        std::int64_t out = 0;
        for (char ch : text) {
            if (!std::isdigit(static_cast<unsigned char>(ch)))
                return false;
            out = (out * 10) + (ch - '0');
        }
        value = out;
        return true;
    }

    bool parseSigned(std::string_view text, int& value) {
        if (text.empty())
            return false;

        bool negative = false;
        std::size_t start = 0;
        if (text[0] == '+' || text[0] == '-') {
            negative = (text[0] == '-');
            start = 1;
        }
        if (start >= text.size())
            return false;

        int out = 0;
        for (std::size_t i = start; i < text.size(); ++i) {
            char ch = text[i];
            if (!std::isdigit(static_cast<unsigned char>(ch)))
                return false;
            out = (out * 10) + (ch - '0');
        }
        value = negative ? -out : out;
        return true;
    }

    template <typename T>
    bool assignWithPresence(qmx::QmxStatus& status, qmx::QmxStatusFlag presentFlag, T& field, const T& value) {
        const bool changed = !status.hasFlag(presentFlag) || field != value;
        status.setFlag(presentFlag);
        field = value;
        return changed;
    }


    // Decode mode from Kenwood 480 "IF" or "MD" response.
    bool decodeModeChar(char modeChar, qmx::QmxMode& modeOut) {
        switch (static_cast<char>(std::toupper(static_cast<unsigned char>(modeChar)))) {
        case '1':
        case 'L':
            modeOut = qmx::QmxMode::LSB;
            return true;
        case '2':
        case 'U':
            modeOut = qmx::QmxMode::USB;
            return true;
        case '3':
        case 'C':
            modeOut = qmx::QmxMode::CW;
            return true;
        case '4':
        case 'F':
            modeOut = qmx::QmxMode::FM; // Parsed for Kenwood MD compatibility; QMX does not use this yet.
            return true;
        case '5':
        case 'A':
            modeOut = qmx::QmxMode::AM;
            return true;
        case '6':
        case 'D':
            modeOut = qmx::QmxMode::FSK;
            return true;
        case '7':
            modeOut = qmx::QmxMode::CWR;
            return true;
        case '9':
            modeOut = qmx::QmxMode::FSKR;
            return true;
        default:
            return false;
        }
    }
}

namespace qmx::detail {

    bool encodeModeCommand(qmx::QmxMode mode, std::string& command) {
        switch (mode) {
        case qmx::QmxMode::LSB:
            command = "MD1;";
            return true;
        case qmx::QmxMode::USB:
            command = "MD2;";
            return true;
        case qmx::QmxMode::CW:
            command = "MD3;";
            return true;
        case qmx::QmxMode::AM:
            command = "MD5;";
            return true;
        case qmx::QmxMode::FSK:
            command = "MD6;";
            return true;
        case qmx::QmxMode::CWR:
            command = "MD7;";
            return true;
        case qmx::QmxMode::FSKR:
            command = "MD9;";
            return true;
        case qmx::QmxMode::FM: // QMX has no FM mode to switch to.
        case qmx::QmxMode::UNKNOWN:
        default:
            return false;
        }
    }

    void QmxCatStatusParser::reset() {
        std::lock_guard<std::mutex> lock(mutex);
        parserBuffer.clear();
        status = {};
        pendingMenuValue = PendingMenuValue::None;
        batchDirty = false;
        batchHadIf = false;
        batchExpectedResponses = 0;
        batchRepliesSeen = 0;
#if QMX_CAT_DEBUG_TIMING
        nextIfSequence = 1;
        pendingIfSequence = 0;
        pendingIfWriteStartUs = 0;
        pendingIfWriteDoneUs = 0;
        pendingIfSendUs = 0;
#endif
    }

    void QmxCatStatusParser::beginBatch(std::size_t expectedResponses) {
        std::lock_guard<std::mutex> lock(mutex);
        // Preserve externally invalidated fields until the batch is published.
        batchHadIf = false;
        batchExpectedResponses = expectedResponses;
        batchRepliesSeen = 0;
    }

    void QmxCatStatusParser::publishBatch() {
        StatusCallback callback = nullptr;
        void* ctx = nullptr;
        QmxStatus snapshot;
        bool shouldPublish = false;

        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = statusCallback;
            ctx = statusCtx;
            shouldPublish = batchHadIf || batchDirty;
            if (shouldPublish) {
                status.sequence++;
                snapshot = status;
            }
            batchDirty = false;
            batchHadIf = false;
        }

        if (shouldPublish && callback) {
            callback(snapshot, ctx);
        }
    }

    bool QmxCatStatusParser::batchComplete() const {
        std::lock_guard<std::mutex> lock(mutex);
        return batchExpectedResponses != 0 && batchRepliesSeen >= batchExpectedResponses;
    }

    void QmxCatStatusParser::setStatusCallback(StatusCallback callback, void* ctx) {
        std::lock_guard<std::mutex> lock(mutex);
        statusCallback = callback;
        statusCtx = ctx;
    }

#if QMX_CAT_RAW_LOG
    void QmxCatStatusParser::setCatLogCallback(CatLogCallback callback, void* ctx) {
        std::lock_guard<std::mutex> lock(mutex);
        catLogCallback = callback;
        catLogCtx = ctx;
    }

    void QmxCatStatusParser::noteError(const std::string& text) {
        CatLogCallback callback = nullptr;
        void* ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = catLogCallback;
            ctx = catLogCtx;
        }
        if (callback) {
            QmxCatLogEntry entry;
            entry.type = QmxCatLogType::Error;
            entry.timestampUs = qmxCatDebugNowUs();
            entry.text = text;
            callback(entry, ctx);
        }
    }
#endif

    void QmxCatStatusParser::armPendingMenuValue(PendingMenuValue value) {
        std::lock_guard<std::mutex> lock(mutex);
        pendingMenuValue = value;
    }

    void QmxCatStatusParser::clearPendingMenuValue() {
        std::lock_guard<std::mutex> lock(mutex);
        pendingMenuValue = PendingMenuValue::None;
    }

    bool QmxCatStatusParser::hasPendingMenuValue() const {
        std::lock_guard<std::mutex> lock(mutex);
        return pendingMenuValue != PendingMenuValue::None;
    }

#if QMX_CAT_DEBUG_TIMING || QMX_CAT_RAW_LOG
    void QmxCatStatusParser::noteWrite(const std::string& command, std::uint64_t writeStartUs, std::uint64_t writeDoneUs, std::uint64_t flushDoneUs) {
#if QMX_CAT_RAW_LOG
        CatLogCallback callback = nullptr;
        void* ctx = nullptr;
#endif
        {
            std::lock_guard<std::mutex> lock(mutex);
#if QMX_CAT_DEBUG_TIMING
            if (command.find("IF;") != std::string::npos) {
                pendingIfSequence = nextIfSequence++;
                pendingIfWriteStartUs = writeStartUs;
                pendingIfWriteDoneUs = writeDoneUs;
                pendingIfSendUs = flushDoneUs;
            }
#endif
#if QMX_CAT_RAW_LOG
            callback = catLogCallback;
            ctx = catLogCtx;
#endif
        }
#if QMX_CAT_RAW_LOG
        if (callback) {
            QmxCatLogEntry entry;
            entry.type = QmxCatLogType::Tx;
            entry.timestampUs = flushDoneUs;
            entry.text = command;
            callback(entry, ctx);
        }
#endif
    }
#endif

    void QmxCatStatusParser::clearStatusFlags(QmxStatusFlags flags) {
        if (flags == 0)
            return;

        std::lock_guard<std::mutex> lock(mutex);
        const QmxStatusFlags oldFlags = status.flags;
        status.clearFlags(flags);
        // Don't report cache invalidation to the upstream, it keeps its own copy which it updates
        // when sending to QMX.
//        if (status.flags != oldFlags)
//            batchDirty = true;
    }

    void QmxCatStatusParser::feedBytes(const char* data, std::size_t count) {
        if (!data || count == 0)
            return;

#if QMX_CAT_RAW_LOG
        std::vector<QmxCatLogEntry> catLogEntries;
        CatLogCallback logCallback = nullptr;
        void* logCtx = nullptr;
#endif

        {
            std::lock_guard<std::mutex> lock(mutex);
#if QMX_CAT_RAW_LOG
            logCallback = catLogCallback;
            logCtx = catLogCtx;
#endif

            for (std::size_t i = 0; i < count; ++i) {
                unsigned char ch = static_cast<unsigned char>(data[i]);
                if (ch == ';') {
                    if (parserBuffer.empty())
                        continue;

                    std::string response;
                    response.swap(parserBuffer);
                    ++batchRepliesSeen;
#if QMX_CAT_RAW_LOG
                    QmxCatLogEntry entry;
                    entry.type = (response == "?") ? QmxCatLogType::Error : QmxCatLogType::Rx;
                    entry.timestampUs = qmxCatDebugNowUs();
                    entry.text = response + ";";
                    catLogEntries.push_back(std::move(entry));
#endif
                    processResponseLocked(response);
                    continue;
                }

                if (ch < 32)
                    // Ignore control characters.
                    // These are illegal in CAT communication and they should not appear.
                    continue;

                parserBuffer.push_back(static_cast<char>(ch));
                if (parserBuffer.size() > 128)
                    // Prevent unbounded parser buffer growth in case of malformed CAT responses.
                    parserBuffer.erase(0, parserBuffer.size() - 64);
            }
        }

#if QMX_CAT_RAW_LOG
        if (logCallback) {
            for (const auto& entry : catLogEntries) {
                logCallback(entry, logCtx);
            }
        }
#endif
    }

    QmxStatus QmxCatStatusParser::snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return status;
    }

    bool QmxCatStatusParser::processResponseLocked(const std::string& response) {
#if QMX_CAT_DEBUG_TIMING
        const std::uint64_t replyUs = qmxCatDebugNowUs();
#endif
        if (response == "?") {
            // A generic "error" response. We don't know whether such response was for menu.
            pendingMenuValue = PendingMenuValue::None;
            return false;
        }
        if (response.size() < 2)
            // Malformed response. Ignore.
            return false;

        const std::string code = response.substr(0, 2);
        const std::string payload = response.substr(2);
        bool changed = false;

        auto valid_bool_response = [&payload]() {
            return !payload.empty() && (payload[0] == '0' || payload[0] == '1');
        };

        // not implemented:
        // AG: Get/Set AF Gain (volume)
        // C2: Get/Set Signal Generator frequency

        //FIXME implement:
        // VN: Returns firmware version
        
        if (code == "IF") {
            // IF: Get transceiver information (TS-480 format)
            batchHadIf = true;

            if (std::int64_t frequency = 0; 
                payload.size() >= 11 && parseUnsigned(std::string_view(payload).substr(0, 11), frequency))
                changed |= assignWithPresence(status, QmxStatusFlag::Frequency, status.frequency, frequency);
            if (int ritHz = 0; 
                payload.size() >= 21 && parseSigned(std::string_view(payload).substr(16, 5), ritHz))
                changed |= assignWithPresence(status, QmxStatusFlag::Rit, status.ritHz, ritHz);
            if (payload.size() > 21 && (payload[21] == '0' || payload[21] == '1'))
                changed |= assignWithPresence(status, QmxStatusFlag::RitEnabled, status.ritEnabled, payload[21] == '1');
            if (payload.size() > 26 && (payload[26] == '0' || payload[26] == '1'))
                changed |= assignWithPresence(status, QmxStatusFlag::Transmit, status.transmit, payload[26] == '1');
            if (payload.size() > 27) {
                QmxMode mode = QmxMode::UNKNOWN;
                if (decodeModeChar(payload[27], mode))
                    changed |= assignWithPresence(status, QmxStatusFlag::Mode, status.mode, mode);
            }
            if (payload.size() > 28 && (payload[28] == '0' || payload[28] == '1'))
                changed |= assignWithPresence(status, QmxStatusFlag::RxVfo, status.rxVfo, payload[28] - '0');
            if (payload.size() > 30 && (payload[30] == '0' || payload[30] == '1'))
                changed |= assignWithPresence(status, QmxStatusFlag::Split, status.split, payload[30] == '1');
#if QMX_CAT_DEBUG_TIMING
            status.catDebug.ifSequence = pendingIfSequence ? pendingIfSequence : nextIfSequence++;
            status.catDebug.ifWriteStartUs = pendingIfWriteStartUs;
            status.catDebug.ifWriteDoneUs = pendingIfWriteDoneUs;
            status.catDebug.ifFlushDoneUs = pendingIfSendUs;
            status.catDebug.ifSendUs = pendingIfSendUs;
            status.catDebug.ifReplyUs = replyUs;
            status.catDebug.ifParsedUs = qmxCatDebugNowUs();
            pendingIfSequence = 0;
            pendingIfWriteStartUs = 0;
            pendingIfWriteDoneUs = 0;
            pendingIfSendUs = 0;
#endif
        }
        else if (code == "FA") {
            // FA: Get/Set VFO A frequency
            if (std::int64_t frequency = 0; parseUnsigned(payload, frequency))
                changed |= assignWithPresence(status, QmxStatusFlag::VfoAFrequency, status.vfoAFrequency, frequency);
        }
        else if (code == "FB") {
            // FB: Get/Set VFO B frequency
            if (std::int64_t frequency = 0; parseUnsigned(payload, frequency))
                changed |= assignWithPresence(status, QmxStatusFlag::VfoBFrequency, status.vfoBFrequency, frequency);
        }
        else if (code == "FR") {
            // FR: Get/Set Receive VFO Mode (0 = VFO A, 1 = VFO B, 2= Split)
            if (valid_bool_response())
                changed |= assignWithPresence(status, QmxStatusFlag::RxVfo, status.rxVfo, payload[0] - '0');
        }
        else if (code == "FT") {
            // FT: Get/Set Transmit VFO Mode (0 = VFO A, 1 = VFO B, 2= Split)
            if (valid_bool_response())
                changed |= assignWithPresence(status, QmxStatusFlag::TxVfo, status.txVfo, payload[0] - '0');
        }
        else if (code == "SP") {    
            if (valid_bool_response())
                changed |= assignWithPresence(status, QmxStatusFlag::Split, status.split, payload[0] == '1');
        }
        else if (code == "TQ") {
            // TQ: Get/Set transmit state (0 = receive, 1 = transmit)
            // Not polled, because IF response contains transmit state in TS-480 emulation mode.
            if (valid_bool_response())
                changed |= assignWithPresence(status, QmxStatusFlag::Transmit, status.transmit, payload[0] == '1');
        }
//        else if (code == "Q1") {
//            if (valid_bool_response())
//                changed |= assignWithPresence(status.hasSideband, status.sideband, (payload[0] == '1') ? QmxSideband::LSB : QmxSideband::USB);
//        }
        else if (code == "MD") {
            // MD: Get/Set operating mode
            if (!payload.empty()) {
                QmxMode mode = QmxMode::UNKNOWN;
                if (decodeModeChar(payload[0], mode))
                    changed |= assignWithPresence(status, QmxStatusFlag::Mode, status.mode, mode);
            }
        }
        else if (code == "MM") {
            // MM: Get/Set/Query menu item
            if (pendingMenuValue == PendingMenuValue::CwOffset) {
                if (int value = 0; parseSigned(payload, value))
                    changed |= assignWithPresence(status, QmxStatusFlag::CwOffset, status.cwOffsetHz, value);
                pendingMenuValue = PendingMenuValue::None;
            }
        } else if (code == "SM") {
            // SM: Get the S-meter value
            if (int value = 0; parseSigned(payload, value))
                changed |= assignWithPresence(status, QmxStatusFlag::SMeter, status.sMeterDb, value);
        } else if (code == "PC") {
            // PC: Get power output
            if (std::int64_t value = 0; parseUnsigned(payload, value))
                changed |= assignWithPresence(status, QmxStatusFlag::Power, status.powerTenthsW, static_cast<int>(value));
        } else if (code == "SW") {
            // SW: Get the SWR-meter value
            if (payload.empty()) {
                if (status.hasSWR()) {
                    status.clearFlag(QmxStatusFlag::SWR);
                    changed = true;
                }
            } else if (std::int64_t value = 0; parseUnsigned(payload, value))
                changed |= assignWithPresence(status, QmxStatusFlag::SWR, status.swrHundredths, static_cast<int>(value));
        }

        if (changed)
            batchDirty = true;
        return changed;
    }
}
