#pragma once
#include <dsp/block.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/processor.h>
#include "arrays.h"
#include <utils/flog.h>
#include <signal_path/signal_path.h>
#include <atomic>
#include <chrono>
#include "logmmse.h"

namespace dsp {

    using namespace ::dsp::arrays;
    using namespace ::dsp::logmmse;

    // Monotonic: feeds the CPU-usage watchdog, which must not trip on wall-clock jumps.
    inline long long currentTimeMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    class IFNRLogMMSE : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;

    public:
        void setInput(stream<complex_t>* in) override {
            if (!_block_init) {
                init(in);
            }
            else {
                Processor::setInput(in);
            }
        }

    public:
        ComplexArray worker1c;
        int freq = 192000;
        LogMMSE::SavedParamsC params;

        void doStart() override {
            base_type::doStart();
            shouldReset = true;
        }

        void setDisableCpuDeactivation(bool disable) {
            disableCpuDeactivation = disable;
        }

        // Set from the retune handler (UI / rigctl thread) and doStart();
        // consumed by process() on the DSP thread.
        std::atomic<bool> shouldReset{ true };
        void reset() {
            shouldReset = true;
        }

        // IN-PLACE: safe (in == out).
        void process(complex_t* in, int count, complex_t* out, int& outCount) {
            // Detect sample rate / decimation changes: the noise profile is
            // only valid for the rate it was sampled at.
            int curFreq = (int)sigpath::iqFrontEnd.getSampleRate();
            if (curFreq != freq) { shouldReset = true; }
            if (shouldReset.exchange(false)) {
                flog::debug("Resetting IF NR LogMMSE");
                worker1c.clear();
                params.reset();
                freq = curFreq;
            }
            worker1c.insert(worker1c.end(), in, in + count);
            int noiseFrames = 12;
            int fram = freq / 100;
            int initialDemand = fram * 2;
            if (params.Xk_prev.empty()) {
                initialDemand = fram * (noiseFrames + 2) * 2;
            }
            if (worker1c.size() < initialDemand) {
                outCount = 0;
                return;
            }
            if (params.Xk_prev.empty()) {
                flog::debug("IF NR LogMMSE: sampling noise profile");
                LogMMSE::logmmse_sample(worker1c, freq, &params, noiseFrames);
            }

            // logmmse_all() can flush a large startup/backlog burst. Bound one
            // pass so we never write past the fixed stream buffer: with
            // Slen >= fram * 2 the output is at most maxInput - Slen samples.
            int maxProcessInput = STREAM_BUFFER_SIZE + (fram * 2);
            auto rv = LogMMSE::logmmse_all(worker1c, &params, maxProcessInput);

            int limit = (int)rv.size();
            auto dta = rv.data();
            for (int i = 0; i < limit; i++) {
                auto lp = dta[i];
                out[i] = lp * 4.0;
            }
            worker1c.erase(worker1c.begin(), worker1c.begin() + limit);
            outCount = limit;
        }

        long long lastReport = currentTimeMillis();
        long long cpuUsed = 0;
        int percentUsage = 0;
        int prevPercentUsage = 0;

        int runMMSE(stream<complex_t>* _in, stream<complex_t>& out) {
            int count = _in->read();
            if (count < 0) { return -1; }
            int outCount;
            long long ctm0 = currentTimeMillis();
            process(_in->readBuf, count, out.writeBuf, outCount);
            long long ctm = currentTimeMillis();
            _in->flush();
            if (!out.swap(outCount)) {
                return -1;
            }
            cpuUsed += ctm - ctm0;
            if (lastReport / 400 != ctm / 400) {
                auto timeSinceLastReport = ctm - lastReport;
                auto usedSinceLastReport = cpuUsed;
                cpuUsed = 0;
                lastReport = ctm;
                prevPercentUsage = percentUsage;
                percentUsage = (usedSinceLastReport * 100) / timeSinceLastReport;
                if (prevPercentUsage >= 95 && percentUsage >= 95 && !disableCpuDeactivation) {
                    stopReason = "Slow CPU. Reduce sample rate.";
                }
            }
            return 1;
        }

        int run() override {
            return runMMSE(_in, out);
        }

        void start() override {
            txHandler.ctx = this;
            txHandler.handler = [](bool txActive, void* ctx) {
                auto _this = (IFNRLogMMSE*)ctx;
                _this->params.hold = txActive;
            };
            sigpath::txState.bindHandler(&txHandler);
            block::start();
        }
        void stop() override {
            percentUsage = -1;
            block::stop();
            sigpath::txState.unbindHandler(&txHandler);
        }

        bool bypass = true;
        // Written by the DSP thread, read by the UI thread every frame; points
        // to a string literal (or nullptr) so the load is a single atomic read.
        std::atomic<const char*> stopReason { nullptr };
        bool disableCpuDeactivation = false;
        EventHandler<bool> txHandler;
    };


}