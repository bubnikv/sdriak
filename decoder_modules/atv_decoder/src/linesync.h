#pragma once
#include <dsp/processor.h>
#include <dsp/buffer/history_buffer.h>
#include <dsp/loop/phase_control_loop.h>
#include <dsp/taps/windowed_sinc.h>
#include <dsp/multirate/polyphase_bank.h>
#include <dsp/math/step.h>

#define LINE_SIZE       945

#define SYNC_LEN        70
#define SYNC_SIDE_LEN   17
#define SYNC_L_START    (LINE_SIZE - SYNC_SIDE_LEN)
#define SYNC_R_START    (SYNC_LEN/2)
#define SYNC_R_END      (SYNC_R_START + (SYNC_LEN/2) + SYNC_SIDE_LEN)
#define SYNC_HALF_LEN   ((SYNC_LEN/2) + SYNC_SIDE_LEN)

#define EQUAL_LEN       35

#define HBLANK_START    SYNC_LEN
#define HBLANK_END      155
#define HBLANK_LEN      (HBLANK_END - HBLANK_START + 1)

#define SYNC_LEVEL      (-0.428)

#define COLORBURST_START    84
#define COLORBURST_LEN      33

#define MAX_LOCK    1000

dsp::complex_t PHASE_REF[2] = {
    { -0.707106781186547f,  0.707106781186547f },
    { -0.707106781186547f, -0.707106781186547f }
};

class LineSync : public dsp::Processor<float, float> {
    using base_type = dsp::Processor<float, float>;
public:
    LineSync() {}

    LineSync(dsp::stream<float>* in, double omega, double omegaGain, double muGain, double omegaRelLimit, int interpPhaseCount = 128, int interpTapCount = 8) { init(in, omega, omegaGain, muGain, omegaRelLimit, interpPhaseCount, interpTapCount); }

    ~LineSync() {
        // The owning members below are destroyed before ~block() runs, so the
        // worker must be joined here.
        if (!base_type::_block_init) { return; }
        base_type::stop();
    }

    void init(dsp::stream<float>* in, double omega, double omegaGain, double muGain, double omegaRelLimit, int interpPhaseCount = 128, int interpTapCount = 8) {
        _omega = omega;
        _omegaGain = omegaGain;
        _muGain = muGain;
        _omegaRelLimit = omegaRelLimit;
        _interpPhaseCount = interpPhaseCount;
        _interpTapCount = interpTapCount;

        // Acquire both before committing either: alloc() throws on OOM and a
        // completed bank would otherwise leak.
        {
            dsp::multirate::PolyphaseBank<float> freshBank = buildInterpBank(_interpPhaseCount, _interpTapCount);
            // The input is a stream block, so STREAM_BUFFER_SIZE bounds it.
            hist.init(STREAM_BUFFER_SIZE, _interpTapCount - 1);
            interpBank = std::move(freshBank);
        }

        // TODO: Needs tuning, so do the gains
        maxPeriod = (int32_t)(1.0001 * (float)(1 << 30));
        minPeriod = (int32_t)(0.9999 * (float)(1 << 30));
    
        base_type::init(in);
    }

    void setInterpParams(int interpPhaseCount, int interpTapCount) {
        assert(base_type::_block_init);
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        dsp::block::TempStop guard(this);
        // Build everything from the *arguments* and commit the dimensions only
        // once both allocations have succeeded. Committing them first and then
        // throwing would let TempStop restart the worker with the new phase/tap
        // counts indexing the old bank and buffer.
        dsp::multirate::PolyphaseBank<float> freshBank = buildInterpBank(interpPhaseCount, interpTapCount);
        // allocate() acquires before it releases, so a throw here leaves the
        // block on its existing buffer rather than empty.
        // Strong, and discards the history: it belonged to the old taps.
        hist.resize(STREAM_BUFFER_SIZE, interpTapCount - 1);

        interpBank = std::move(freshBank);
        _interpPhaseCount = interpPhaseCount;
        _interpTapCount = interpTapCount;
    }

    void reset() {
        assert(base_type::_block_init);
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        dsp::block::TempStop guard(this);
        offset = 0;
        phase = 0;
    }

    int run() {
        int count = base_type::_in->read();
        if (count < 0) { return -1; }

        const float* window = hist.push(base_type::_in->readBuf, count);

        // Process samples while they are available
        while (offset < count) {
            // While the offset is negative, out put zeros
            while (offset < 0 && pixel < LINE_SIZE) {
                // Output a zero
                base_type::out.writeBuf[pixel++] = 0.0f;

                // Increment the phase
                phase += period;
                offset += (phase >> 30);
                phase &= 0x3FFFFFFF;
            }

            // Process as much of a line as possible
            while (offset < count && pixel < LINE_SIZE) {
                // Compute the output sample
                volk_32f_x2_dot_prod_32f(&base_type::out.writeBuf[pixel++], &window[offset], interpBank.phase((phase >> 23) & 0x7F), _interpTapCount);
                
                // Increment the phase
                phase += period;
                offset += (phase >> 30);
                phase &= 0x3FFFFFFF;
            }

            // If the line is done, process it
            if (pixel == LINE_SIZE) {
                // Compute averages. (TODO: Try faster method)
                float left = 0.0f, right = 0.0f;
                int lc = 0, rc = 0;
                for (int i = SYNC_L_START; i < LINE_SIZE; i++) {
                    left += base_type::out.writeBuf[i];
                    lc++;
                }
                for (int i = 0; i < SYNC_R_START; i++) {
                    left += base_type::out.writeBuf[i];
                    lc++;
                }
                for (int i = SYNC_R_START; i < SYNC_R_END; i++) {
                    right += base_type::out.writeBuf[i];
                    rc++;
                }

                // Compute the error
                float error = (left - right) * (1.0f/((float)SYNC_HALF_LEN));

                // Compute the change in phase and frequency due to the error
                float periodDelta = error * _omegaGain;
                float phaseDelta = error * _muGain;

                // Normalize the phase delta (TODO: Make faster)
                while (phaseDelta <= -1.0f) {
                    phaseDelta += 1.0f;
                    offset--;
                }
                while (phaseDelta >= 1.0f) {
                    phaseDelta -= 1.0f;
                    offset++;
                }

                // Update the period (TODO: Clamp error*omegaGain to prevent weird shit with corrupt samples)
                period += (int32_t)(periodDelta * (float)(1 << 30));
                period = std::clamp<uint32_t>(period, minPeriod, maxPeriod);

                // Update the phase
                phase += (int32_t)(phaseDelta * (float)(1 << 30));

                // Normalize the phase
                uint32_t overflow = phase >> 30;
                if (overflow) {
                    if (error < 0) {
                        offset -= 4 - overflow;
                    }
                    else {
                        offset += overflow;
                    }
                }
                phase &= 0x3FFFFFFF;

                // Find the lowest value
                float lowest = INFINITY;
                int lowestId = -1;
                for (int i = 0; i < LINE_SIZE; i++) {
                    float val =  base_type::out.writeBuf[i];
                    if (val < lowest) {
                        lowest = val;
                        lowestId = i;
                    }
                }

                // Check the the line is in lock
                bool lineLocked = (lowestId < SYNC_R_END || lowestId >= SYNC_L_START);

                // Update the lock status based on the line lock
                if (!lineLocked && locked) {
                    locked--;
                }
                else if (lineLocked && locked < MAX_LOCK) {
                    locked++;
                }

                // If not locked, attempt to lock by forcing the sync to happen at the right spot
                // TODO: This triggers waaaay too easily at low SNR
                if (!locked && fastLock) {
                    offset += lowestId - SYNC_R_START;
                    locked = MAX_LOCK / 2;
                }

                // Output line
                if (!base_type::out.swap(LINE_SIZE)) { break; }
                pixel = 0;
            }
        }

        // Get the offset ready for the next buffer
        offset -= count;

        hist.advance(count);

        // Swap if some data was generated
        base_type::_in->flush();
        return 0;
    }

    float syncBias = 0;

    uint32_t period = (0x800072F3 >> 1);//(1 << 31) + 1;

    int locked = 0;
    bool fastLock = true;

protected:
    // Returns the bank instead of assigning it, and takes the dimensions as
    // arguments rather than reading the members, so callers can build a
    // replacement before committing anything. lp owns its taps: if the bank
    // allocation throws, the windowed sinc is released on the way out.
    dsp::multirate::PolyphaseBank<float> buildInterpBank(int phaseCount, int tapCount) {
        double bw = 0.5 / (double)phaseCount;
        dsp::tap<float> lp = dsp::taps::windowedSinc<float>(phaseCount * tapCount, dsp::math::hzToRads(bw, 1.0), dsp::window::nuttall, phaseCount);
        return dsp::multirate::buildPolyphaseBank<float>(phaseCount, lp);
    }

    dsp::multirate::PolyphaseBank<float> interpBank;

    double _omega;
    double _omegaGain;
    double _muGain;
    double _omegaRelLimit;
    int _interpPhaseCount;
    int _interpTapCount;
    // history: interpTapCount-1 + block <= STREAM_BUFFER_SIZE
    dsp::buffer::HistoryBuffer<float> hist;

    uint32_t phase = 0;
    uint32_t maxPeriod;
    uint32_t minPeriod;
    float syncLevel = -0.03f;

    int offset = 0;
    int pixel = 0;
};