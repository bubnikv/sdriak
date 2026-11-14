#pragma once
#include <assert.h>
#include <string.h>
#include <utility>
#include <volk/volk.h>
#include "../types.h"
#include "../buffer/buffer.h"
#include "../buffer/history_buffer.h"
#include "../taps/tap.h"
#include "../processor.h"

namespace dsp::filter {
    // FIR Filter computation kernel.
    //
    // Tap ownership: init()/setTaps() taking an rvalue (a freshly generated
    // set, or std::move(t)) adopt it and free it when replaced or destroyed.
    // Taking an lvalue they only borrow it, and the caller must keep it alive,
    // unchanged, for as long as the FIR uses it. Prefer handing ownership over;
    // borrowing exists for sets shared with something else.
    template <class D, class T>
    class FIR {
    public:
        FIR() = default;

        FIR(tap<T>&& taps, int maxBlock = STREAM_BUFFER_SIZE) { init(std::move(taps), maxBlock); }
        FIR(const tap<T>& taps, int maxBlock = STREAM_BUFFER_SIZE) { init(taps, maxBlock); }

        void init(tap<T>&& taps, int maxBlock = STREAM_BUFFER_SIZE) {
            // Adopted only once initTaps() has succeeded: on a throw the
            // caller's temporary still owns and frees the set.
            initTaps(taps, maxBlock);
            m_ownedTaps = std::move(taps);
        }

        void init(const tap<T>& taps, int maxBlock = STREAM_BUFFER_SIZE) {
            initTaps(taps, maxBlock);
            m_ownedTaps.release();
        }

        // maxBlock defaults to -1, meaning "keep the current delay line".
        // Pass a value when the caller's block bound has changed -- a filter
        // that runs after a resampler sees a fraction of the block its owner
        // was sized for, and the delay line is the expensive part. Capacity
        // changes preserve the newest history; callers whose sample domain
        // changed must reset explicitly after publishing the new taps.
        //
        // The previous set, if owned, is freed only after the new one is
        // published, so the FIR never points at freed taps.
        void setTaps(tap<T>&& taps, int maxBlock = -1) {
            applyTaps(taps, maxBlock);
            m_ownedTaps = std::move(taps);
        }

        void setTaps(const tap<T>& taps, int maxBlock = -1) {
            applyTaps(taps, maxBlock);
            m_ownedTaps.release();
        }

        // Resize the delay line for a new block bound, keeping both the taps
        // and retained samples. A capacity change by itself is not a signal
        // discontinuity.
        void setMaxBlock(int maxBlock) {
            assert(m_init);
            m_hist.setMaxBlock(maxBlock);
        }

        int tapCount() const { return m_taps.size(); }
        int maxBlock() const { return m_hist.maxBlock(); }
        bool ownsTaps() const { return m_ownedTaps.taps() != NULL; }

        // The set this FIR owns, for another FIR to borrow. The borrower must
        // be re-pointed (setTaps()) whenever this one's taps are replaced, and
        // must not outlive it.
        const tap<T>& ownedTaps() const {
            assert(ownsTaps());
            return m_ownedTaps;
        }

        virtual void reset() {
            assert(m_init);
            m_hist.clear();
        }

        // The delay line keeps the last taps-1 input samples; see
        // buffer::HistoryBuffer for the layout and why the input is copied.
        // IN-PLACE: safe (in == out).
        inline int process(int count, const D* in, D* out) {
            const D* window = m_hist.push(in, count);

            // Do convolution
            for (int i = 0; i < count; i++) {
                if constexpr (std::is_same_v<D, float> && std::is_same_v<T, float>)
                    volk_32f_x2_dot_prod_32f(&out[i], &window[i], m_taps.taps(), m_taps.size());
                else if constexpr ((std::is_same_v<D, complex_t> || std::is_same_v<D, stereo_t>) && std::is_same_v<T, float>)
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&out[i], (const lv_32fc_t*)&window[i], m_taps.taps(), m_taps.size());
                else if constexpr ((std::is_same_v<D, complex_t> || std::is_same_v<D, stereo_t>) && std::is_same_v<T, complex_t>)
                    volk_32fc_x2_dot_prod_32fc((lv_32fc_t*)&out[i], (const lv_32fc_t*)&window[i], (const lv_32fc_t*)m_taps.taps(), m_taps.size());
                else {
                    static_assert(dependent_false_v<D, T>, "Unsupported FIR input/output/tap type combination");
                }
            }

            m_hist.advance(count);
            return count;
        }

    protected:
        void initTaps(TapView<T> taps, int maxBlock) {
            // [RATE-SIZED] Callers may pass a maxBlock derived from a rate
            // ratio rather than STREAM_BUFFER_SIZE; process() then relies on
            // count <= maxBlock. see doc/design/buffer-size-requirements.md 2.6
            // Allocate first, so a throw commits nothing.
            m_hist.init(maxBlock, taps.size() - 1);
            m_taps = taps;
#ifndef NDEBUG
            m_init = true;
#endif
        }

        // Publishes a new active tap set and, if maxBlock > 0, a new block
        // bound. Overridden by DecimatingFIR to reset its phase.
        //
        // The delay line is reshaped before the taps are committed. Each
        // HistoryBuffer operation has the strong guarantee. If growing the
        // history fails after a successful capacity change, the FIR retains
        // its old taps and history with only harmless extra block capacity.
        virtual void applyTaps(TapView<T> taps, int maxBlock) {
            assert(m_init);
            if (maxBlock > 0 && maxBlock != m_hist.maxBlock()) {
                // [RATE-SIZED] Same contract as init(): the new bound comes
                // from the caller's rate arithmetic. This changes capacity,
                // not the sample domain, so keep the retained samples.
                // see doc/design/buffer-size-requirements.md 2.6
                m_hist.setMaxBlock(maxBlock);
            }
            // Keeps the newest samples when the tap count changes, matching
            // the pre-kernel FIR behavior. A caller changing sample domains is
            // responsible for an explicit reset after the new taps commit.
            m_hist.setHistory(taps.size() - 1);
            m_taps = taps;
        }

#ifndef NDEBUG
        bool m_init = false;
#endif
        // The active tap set: points into m_ownedTaps when the FIR owns it,
        // into a caller-kept set when it borrows. Moving the FIR keeps it
        // valid, since the view holds the data pointer, not the tap object.
        TapView<T> m_taps;
        // Non-empty exactly when the FIR owns the set m_taps points at.
        tap<T> m_ownedTaps;
        // [ history: taps-1 | block: <= maxBlock ]
        buffer::HistoryBuffer<D> m_hist;
    };

    // Threaded adapter around the kernel, for the FIRs that really are chain
    // nodes with their own worker and output stream.
    template <class D, class T>
    class FIRBlock : public Processor<D, D> {
        using base_type = Processor<D, D>;
    public:
        FIRBlock() = default;

        FIRBlock(stream<D>* in, tap<T>&& taps) { init(in, std::move(taps)); }
        FIRBlock(stream<D>* in, const tap<T>& taps) { init(in, taps); }

        ~FIRBlock() {
            // The owning members below are destroyed before ~block() runs, so
            // the worker must be joined here -- otherwise it keeps reading
            // buffers that have already been freed.
            if (base_type::_block_init)
                base_type::stop();
        }

        // Same ownership rules as FIR: an rvalue is adopted, an lvalue borrowed.
        void init(stream<D>* in, tap<T>&& taps) {
            kernel.init(std::move(taps));
            base_type::init(in);
        }

        void init(stream<D>* in, const tap<T>& taps) {
            kernel.init(taps);
            base_type::init(in);
        }

        void setTaps(tap<T>&& taps, int maxBlock = -1) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            kernel.setTaps(std::move(taps), maxBlock);
        }

        void setTaps(const tap<T>& taps, int maxBlock = -1) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            kernel.setTaps(taps, maxBlock);
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            kernel.reset();
        }

        // IN-PLACE: safe (in == out). Delegates to the kernel above.
        inline int process(int count, const D* in, D* out) {
            return kernel.process(count, in, out);
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            kernel.process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

        FIR<D, T> kernel;
    };
}
