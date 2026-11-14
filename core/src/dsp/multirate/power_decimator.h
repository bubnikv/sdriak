#pragma once
#include <memory>
#include <vector>
#include "../filter/decimating_fir.h"
#include "../taps/from_array.h"
#include "decim/plans.h"

namespace dsp::multirate {
    // Computation kernel, not a dsp::block. Fused inside RationalResampler;
    // the one real chain node (IQFrontEnd) uses PowerDecimatorBlock below.
    // See dsp::math::Delay for why.
    template<class T>
    class PowerDecimator {
    public:
        PowerDecimator() {}

        PowerDecimator(unsigned int ratio, int maxBlock = STREAM_BUFFER_SIZE) { init(ratio, maxBlock); }

        void init(unsigned int ratio, int maxBlock = STREAM_BUFFER_SIZE) {
            assert(checkRatio(ratio));
            _maxBlock = maxBlock;
            reconfigure(ratio);
            _init = true;
        }

        static inline unsigned int getMaxRatio() {
            return 1 << decim::plans_len;
        }

        void setRatio(unsigned int ratio) {
            assert(_init);
            assert(checkRatio(ratio));
            // reconfigure() commits _ratio only after the stages exist: setting
            // it up front would leave process() using the new ratio against the
            // old stage list if the rebuild threw.
            reconfigure(ratio);
        }

        void reset() {
            assert(_init);
            for (auto& fir : decimFirs) {
                fir->reset();
            }
        }

        // IN-PLACE: safe (in == out). Every stage is an in-place-safe
        // DecimatingFIR, and the ratio-1 copy is skipped when out == in.
        inline int process(int count, const T* in, T* out) {
            // If the ratio is 1, no need to decimate
            if (_ratio == 1) {
                if (out != in) { memcpy(out, in, count * sizeof(T)); }
                return count;
            }
            
            // Process data through each stage
            const T* data = in;
            int last = stageCount - 1;
            for (int i = 0; i < stageCount; i++) {
                count = decimFirs[i]->process(count, data, out);
                data = out;
            }
            return count;
        }


    protected:
        bool _init = false;
        int _maxBlock = STREAM_BUFFER_SIZE;

        // Build the replacement stages into locals and commit only once every
        // allocation has succeeded. Doing it in place would (a) discard a
        // working configuration before the new one exists, and (b) leak the tap
        // set or the FIR if the following allocation threw -- and during the
        // first init() the destructor guard could not clean up either, because
        // _init is still false at that point.
        void reconfigure(unsigned int ratio) {
            std::vector<std::unique_ptr<filter::DecimatingFIR<T, float>>> newFirs;
            int newStageCount = 0;

            if (ratio > 1) {
                int planId = log2(ratio) - 1;
                decim::plan plan = decim::plans[planId];
                newStageCount = plan.stageCount;
                newFirs.reserve(newStageCount);
                // Only the first stage sees a full block. Each one emits at
                // most ceil(in / decimation), so sizing them all for _maxBlock
                // -- which is what this used to do -- pays for the whole chain
                // at the input rate. For plan_32 ({8, 2, 2}) that is three
                // 8.5 MB working buffers where 8.5 + 1.5 + 1.0 will do.
                // [RATE-SIZED] Exact: the decimations come from the plan, so
                // stage i sees at most ceil(prev / decimation). Wrong only if
                // DecimatingFIR ever emits more than that. see doc/design/buffer-size-requirements.md 2.6
                int stageMax = _maxBlock;
                for (int i = 0; i < newStageCount; i++) {
                    const int dec = plan.stages[i].decimation;
                    // The FIR adopts the tap set; if its allocation throws,
                    // the temporary releases it.
                    newFirs.emplace_back(new filter::DecimatingFIR<T, float>(taps::fromArray<float>(plan.stages[i].tapcount, plan.stages[i].taps), dec, stageMax));
                    stageMax = (stageMax + dec - 1) / dec;
                }
            }

            decimFirs = std::move(newFirs);
            stageCount = newStageCount;
            _ratio = ratio;
        }

        bool checkRatio(unsigned int ratio) {
            // Make sure ratio is a power of two, non-zero and lower or equal to maximum
            return ((ratio & (ratio - 1)) == 0) && ratio && ratio <= getMaxRatio();
        }

        std::vector<std::unique_ptr<filter::DecimatingFIR<T, float>>> decimFirs;
        unsigned int _ratio;
        int stageCount;
    };

    // Threaded adapter around the kernel, for the decimator that really is a
    // chain node (the IQ front end's pre-processor).
    template<class T>
    class PowerDecimatorBlock : public Processor<T, T> {
        using base_type = Processor<T, T>;
    public:
        PowerDecimatorBlock() {}

        PowerDecimatorBlock(stream<T>* in, unsigned int ratio) { init(in, ratio); }

        ~PowerDecimatorBlock() {
            // The owning members below are destroyed before ~block() runs, so
            // the worker must be joined here -- otherwise it keeps reading
            // buffers that have already been freed.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        static inline unsigned int getMaxRatio() { return PowerDecimator<T>::getMaxRatio(); }

        void init(stream<T>* in, unsigned int ratio) {
            kernel.init(ratio);
            base_type::init(in);
        }

        void setRatio(unsigned int ratio) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            kernel.setRatio(ratio);
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            kernel.reset();
        }

        // IN-PLACE: safe (in == out). Delegates to the kernel above.
        inline int process(int count, const T* in, T* out) { return kernel.process(count, in, out); }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = kernel.process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

        PowerDecimator<T> kernel;
    };
}
