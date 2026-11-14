#pragma once
#include "../processor.h"
#include "../buffer/history_buffer.h"
#include "../loop/phase_control_loop.h"
#include "../taps/windowed_sinc.h"
#include "../multirate/polyphase_bank.h"
#include "../math/step.h"

namespace dsp::clock_recovery {
    class FD : public Processor<float, float> {
        using base_type = Processor<float, float> ;
    public:
        FD() {}

        FD(stream<float>* in, double omega, double omegaGain, double muGain, double omegaRelLimit, int interpPhaseCount = 128, int interpTapCount = 8) { init(in, omega, omegaGain, muGain, omegaRelLimit, interpPhaseCount, interpTapCount); }

        ~FD() {
            // The owning members below are destroyed before ~block() runs, so
            // the worker must be joined here.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<float>* in, double omega, double omegaGain, double muGain, double omegaRelLimit, int interpPhaseCount = 128, int interpTapCount = 8) {
            _omega = omega;
            _omegaGain = omegaGain;
            _muGain = muGain;
            _omegaRelLimit = omegaRelLimit;
            _interpPhaseCount = interpPhaseCount;
            _interpTapCount = interpTapCount;

            pcl.init(_muGain, _omegaGain, 0.0, 0.0, 1.0, _omega, _omega * (1.0 - omegaRelLimit), _omega * (1.0 + omegaRelLimit));
            // Acquire both before committing either: alloc() throws on OOM and
            // a completed bank would otherwise leak.
            {
                dsp::multirate::PolyphaseBank<float> freshBank = buildInterpBank(_interpPhaseCount, _interpTapCount);
                // The input is a stream block, so STREAM_BUFFER_SIZE bounds it.
                hist.init(STREAM_BUFFER_SIZE, _interpTapCount - 1);
                interpBank = std::move(freshBank);
            }
        
            base_type::init(in);
        }

        void setOmega(double omega) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _omega = omega;
            offset = 0;
            pcl.phase = 0.0f;
            pcl.freq = _omega;
            pcl.setFreqLimits(_omega * (1.0 - _omegaRelLimit), _omega * (1.0 + _omegaRelLimit));
        }

        void setOmegaGain(double omegaGain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _omegaGain = omegaGain;
            pcl.setCoefficients(_muGain, _omegaGain);
        }

        void setMuGain(double muGain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _muGain = muGain;
            pcl.setCoefficients(_muGain, _omegaGain);
        }

        void setOmegaRelLimit(double omegaRelLimit) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _omegaRelLimit = omegaRelLimit;
            pcl.setFreqLimits(_omega * (1.0 - _omegaRelLimit), _omega * (1.0 + _omegaRelLimit));
        }

        void setInterpParams(int interpPhaseCount, int interpTapCount) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            // Build everything from the *arguments* and commit the dimensions
            // only once both allocations have succeeded. Committing them first
            // and then throwing would let TempStop restart the worker with the
            // new phase/tap counts indexing the old bank and buffer.
            dsp::multirate::PolyphaseBank<float> freshBank = buildInterpBank(interpPhaseCount, interpTapCount);
            // allocate() acquires before it releases, so a throw here leaves
            // the block on its existing buffer rather than empty.
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
            pcl.phase = 0.0f;
            pcl.freq = _omega;
        }

        // IN-PLACE: safe (in == out).
        inline int process(int count, const float* in, float* out) {
            const float* window = hist.push(in, count);

            // Process all samples
            int outCount = 0;
            while (offset < count) {
                float error;
                float outVal;
                float dfdt;

                // Calculate new output value
                int phase = std::clamp<int>(floorf(pcl.phase * (float)_interpPhaseCount), 0, _interpPhaseCount - 1);
                volk_32f_x2_dot_prod_32f(&outVal, &window[offset], interpBank.phase(phase), _interpTapCount);
                out[outCount++] = outVal;

                // Calculate derivative of the signal
                if (phase == 0) {
                    float fT1;
                    volk_32f_x2_dot_prod_32f(&fT1, &window[offset], interpBank.phase(phase+1), _interpTapCount);
                    dfdt = fT1 - outVal;
                }
                else if (phase == _interpPhaseCount - 1) {
                    float fT_1;
                    volk_32f_x2_dot_prod_32f(&fT_1, &window[offset], interpBank.phase(phase-1), _interpTapCount);
                    dfdt = outVal - fT_1;
                }
                else {
                    float fT_1;
                    float fT1;
                    volk_32f_x2_dot_prod_32f(&fT_1, &window[offset], interpBank.phase(phase-1), _interpTapCount);
                    volk_32f_x2_dot_prod_32f(&fT1, &window[offset], interpBank.phase(phase+1), _interpTapCount);
                    dfdt = (fT1 - fT_1) * 0.5f;
                }
                
                // Calculate error
                error = dfdt * math::step(outVal);

                // Clamp symbol phase error
                if (error > 1.0f) { error = 1.0f; }
                if (error < -1.0f) { error = -1.0f; }

                // Advance symbol offset and phase
                pcl.advance(error);
                float delta = floorf(pcl.phase);
                offset += delta;
                pcl.phase -= delta;
            }
            offset -= count;

            hist.advance(count);

            return outCount;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            // Swap if some data was generated
            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

        loop::PhaseControlLoop<float, false> pcl;

    protected:
        // Returns the bank instead of assigning it, so callers can build the
        // replacement before releasing the old one. lp owns its taps: if the
        // bank allocation throws, the windowed sinc is released on the way out.
        // Takes the dimensions as arguments rather than reading the members,
        // so callers can build a replacement without having committed them yet.
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

        int offset = 0;
        // [ history: interpTapCount-1 | block: <= STREAM_BUFFER_SIZE ]
        buffer::HistoryBuffer<float> hist;
    };
}