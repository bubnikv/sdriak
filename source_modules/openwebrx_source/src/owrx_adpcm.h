#pragma once
// The two IMA ADPCM framings OpenWebRX servers send: per-row FFT frames, and
// the audio path's stereo stream with sync frames, which carries the IQ.
// Written from the IMA ADPCM algorithm and the framing described in
// doc/research/openwebrx.md section 5; no OpenWebRX code is used.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace owrx {
    namespace adpcm {
        inline constexpr int kIndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
        inline constexpr int kStepTable[89] = {
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
            107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
            796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
            4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
            20350, 22385, 24623, 27086, 29794, 32767
        };

        // One channel's decoder state. step starts at 0, not kStepTable[0],
        // as the servers' own browser client does; the FFT's padding values
        // absorb the difference.
        struct Channel {
            int index = 0;
            int predictor = 0;
            int step = 0;

            int decode(int nibble) {
                index = std::clamp(index + kIndexTable[nibble], 0, 88);
                int diff = step >> 3;
                if (nibble & 1) { diff += step >> 2; }
                if (nibble & 2) { diff += step >> 1; }
                if (nibble & 4) { diff += step; }
                if (nibble & 8) { diff = -diff; }
                predictor = std::clamp(predictor + diff, -32768, 32767);
                step = kStepTable[index];
                return predictor;
            }
        };
    }

    // FFT rows with fft_compression "adpcm": each row decoded from a reset
    // state, low nibble first, the first kPad values dropped, centi-dB.
    struct FftAdpcm {
        static constexpr int kPad = 10;  // COMPRESS_FFT_PAD_N

        // Bytes one row of fftSize bins takes on the wire.
        static size_t rowBytes(int fftSize) { return (size_t)(fftSize + kPad + 1) / 2; }

        // One row into dB; out gets fftSize values (fewer if the row is short).
        static void decodeRow(const uint8_t* data, size_t len, int fftSize, std::vector<float>& out) {
            adpcm::Channel ch;
            out.clear();
            out.reserve((size_t)fftSize);
            int n = 0;
            for (size_t i = 0; i < len && (int)out.size() < fftSize; i++) {
                for (int h = 0; h < 2 && (int)out.size() < fftSize; h++) {
                    const int v = ch.decode(h == 0 ? (data[i] & 15) : (data[i] >> 4));
                    if (n++ >= kPad) { out.push_back((float)v / 100.0f); }
                }
            }
        }
    };

    // The audio path's stereo IMA ADPCM with sync frames (the server's
    // AdpcmEncoder with sync on): "SYNC", four little-endian int16 (step
    // index and predictor of channel 1, then of channel 2), then
    // kPairsPerFrame byte pairs. In a pair the first byte is channel 1 and
    // the second channel 2, each low nibble first. The state persists across
    // messages, and a pair, a header or the sync word may be split between
    // two messages. Bytes outside a frame are skipped until the next "SYNC".
    class StereoSyncAdpcm {
    public:
        static constexpr int kPairsPerFrame = 1001;

        void reset() { *this = StereoSyncAdpcm(); }

        // Appends the decoded samples (two per channel and pair) to a and b.
        void decode(const uint8_t* data, size_t len, std::vector<int16_t>& a, std::vector<int16_t>& b) {
            static constexpr uint8_t kSync[4] = { 'S', 'Y', 'N', 'C' };
            for (size_t k = 0; k < len; k++) {
                const uint8_t x = data[k];
                switch (m_phase) {
                case Phase::Hunt:
                    if (x == kSync[m_matched]) {
                        if (++m_matched == 4) {
                            m_phase = Phase::Header;
                            m_headerLen = 0;
                        }
                    }
                    else {
                        m_matched = (x == kSync[0]) ? 1 : 0;
                    }
                    break;
                case Phase::Header:
                    m_header[m_headerLen++] = x;
                    if (m_headerLen == 8) {
                        auto le16 = [this](int i) { return (int)(int16_t)(m_header[2 * i] | (m_header[2 * i + 1] << 8)); };
                        m_ch[0].index = std::clamp(le16(0), 0, 88);
                        m_ch[0].predictor = le16(1);
                        m_ch[0].step = adpcm::kStepTable[m_ch[0].index];
                        m_ch[1].index = std::clamp(le16(2), 0, 88);
                        m_ch[1].predictor = le16(3);
                        m_ch[1].step = adpcm::kStepTable[m_ch[1].index];
                        m_pairsLeft = kPairsPerFrame;
                        m_haveFirst = false;
                        m_phase = Phase::Data;
                        m_synced = true;
                    }
                    break;
                case Phase::Data:
                    if (!m_haveFirst) {
                        m_first = x;
                        m_haveFirst = true;
                        break;
                    }
                    a.push_back((int16_t)m_ch[0].decode(m_first & 15));
                    a.push_back((int16_t)m_ch[0].decode(m_first >> 4));
                    b.push_back((int16_t)m_ch[1].decode(x & 15));
                    b.push_back((int16_t)m_ch[1].decode(x >> 4));
                    m_haveFirst = false;
                    if (--m_pairsLeft == 0) {
                        m_phase = Phase::Hunt;
                        m_matched = 0;
                    }
                    break;
                }
            }
        }

        // A full sync header was seen since the last reset.
        bool synced() const { return m_synced; }

    private:
        enum class Phase { Hunt, Header, Data };
        Phase m_phase = Phase::Hunt;
        int m_matched = 0;
        uint8_t m_header[8] = {};
        int m_headerLen = 0;
        int m_pairsLeft = 0;
        bool m_haveFirst = false;
        uint8_t m_first = 0;
        bool m_synced = false;
        adpcm::Channel m_ch[2];
    };
}
