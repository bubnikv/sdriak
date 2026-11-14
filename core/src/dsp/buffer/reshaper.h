#pragma once
#include "../block.h"
#include <assert.h>
#include <algorithm>
#include "ring_buffer.h"

// IMPORTANT: THIS IS TRASH AND MUST BE REWRITTEN IN THE FUTURE

namespace dsp::buffer {
    // NOTE: I'm not proud of this, it's BAD and just taken from the previous DSP, but it works...
    template <class T>
    class Reshaper : public block {
        using base_type = block;
    public:
        Reshaper() {}

        Reshaper(stream<T>* in, int keep, int skip, int maxKeep = STREAM_BUFFER_SIZE) {
            init(in, keep, skip, maxKeep);
        }

        // NOTE: For some reason, the base class destructor doesn't get called.... this is a temporary fix I guess
        // I also don't check for _block_init for the exact sample reason, something's weird
        ~Reshaper() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        // maxKeep is the largest value that will ever be passed to setKeep();
        // `out` is sized for it. bufferWorker() publishes exactly _keep samples
        // per block, so the default leaves a `keep`-sample output in a buffer
        // built for a million -- but it is only safe to narrow when the caller
        // knows its keep is bounded, because setKeep() cannot resize the stream
        // afterwards (see there). Callers that vary keep at runtime leave it
        // alone. (The ring buffer is a separate matter: it is sized by
        // RING_BUF_SZ, its own literal, not by maxLatency.)
        void init(stream<T>* in, int keep, int skip, int maxKeep = STREAM_BUFFER_SIZE) {
            _in = in;
            _keep = keep;
            _skip = skip;
            _maxKeep = (std::max)(maxKeep, keep);
            ringBuf.init(keep * 2);
            // [RATE-SIZED] Exact rather than ratio-derived -- bufferWorker()
            // publishes exactly _keep samples -- but `out` is no longer
            // STREAM_BUFFER_SIZE, and setKeep() asserts against this bound
            // because it cannot resize afterwards. see doc/design/buffer-size-requirements.md 2.6
            if (_maxKeep < STREAM_BUFFER_SIZE) { out.setBufferSize(_maxKeep); }
            base_type::registerInput(_in);
            base_type::registerOutput(&out);
            base_type::_block_init = true;
        }

        void setInput(stream<T>* in) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            base_type::unregisterInput(_in);
            _in = in;
            base_type::registerInput(_in);
        }

        void setKeep(int keep) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            // `out` is deliberately not resized here. TempStop stops this
            // block, not whatever reads `out`, and setBufferSize() frees the
            // buffer a running consumer may still be holding --
            // iq_exporter calls this straight from its menu handler. Growth
            // past maxKeep is the caller's contract, asserted below.
            assert(keep <= _maxKeep);
            _keep = keep;
            ringBuf.setMaxLatency(keep * 2);
        }

        int getKeepBound() const { return _maxKeep; }

        // Moves the bound `out` is sized for, which setKeep() cannot do.
        //
        // PRECONDITION 1: the caller must have stopped whatever reads `out`.
        // TempStop below stops this block, not the reader, and setBufferSize()
        // frees the buffer that reader may still hold. IQFrontEnd::
        // updateFFTPath() holds a TempStop on both this block and its sink,
        // which is why it may call this and iq_exporter may not.
        //
        // PRECONDITION 2: this allocates, so the caller has to order it against
        // its own setKeep() by direction -- grow the bound before _keep rises,
        // shrink it after _keep has fallen. Either crossing leaves `out` too
        // small for the block the worker publishes the moment it restarts, and
        // the caller's reconfiguration can throw in between. See
        // IQFrontEnd::updateFFTPath(), the only caller.
        void setKeepBound(int maxKeep) {
            assert(base_type::_block_init);
            assert(maxKeep > 0);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            if (maxKeep == _maxKeep) { return; }
            // Commit the bound only once `out` actually has it. setBufferSize()
            // is strongly exception-safe, but assigning first would, on a throw,
            // leave _maxKeep claiming a capacity the stream does not have -- and
            // setKeep()'s assert against it would then wave through a keep that
            // overruns the buffer.
            out.setBufferSize(maxKeep);
            _maxKeep = maxKeep;
        }

        void setSkip(int skip) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _skip = skip;
        }

        int run() override {
            int count = _in->read();
            if (count < 0) { return -1; }
            ringBuf.write(_in->readBuf, count);
            _in->flush();
            return count;
        }

        stream<T> out;

    private:
        void doStart() override {
            workThread = std::thread(&Reshaper<T>::loop, this);
            bufferWorkerThread = std::thread(&Reshaper<T>::bufferWorker, this);
        }

        void loop() {
            while (run() >= 0)
                ;
        }

        void doStop() override {
            _in->stopReader();
            ringBuf.stopReader();
            out.stopWriter();
            ringBuf.stopWriter();

            if (workThread.joinable()) {
                workThread.join();
            }
            if (bufferWorkerThread.joinable()) {
                bufferWorkerThread.join();
            }

            _in->clearReadStop();
            ringBuf.clearReadStop();
            out.clearWriteStop();
            ringBuf.clearWriteStop();
        }

        void bufferWorker() {
            T* buf = new T[_keep];
            bool delay = _skip < 0;

            int readCount = std::min<int>(_keep + _skip, _keep);
            int skip = std::max<int>(_skip, 0);
            int delaySize = (-_skip) * sizeof(T);
            int delayCount = (-_skip);

            T* start = &buf[std::max<int>(-_skip, 0)];
            T* delayStart = &buf[_keep + _skip];

            while (true) {
                if (delay) {
                    memmove(buf, delayStart, delaySize);
                    if constexpr (std::is_same_v<T, complex_t> || std::is_same_v<T, stereo_t>) {
                        for (int i = 0; i < delayCount; i++) {
                            buf[i].re /= 10.0f;
                            buf[i].im /= 10.0f;
                        }
                    }
                }
                if (ringBuf.readAndSkip(start, readCount, skip) < 0) { break; };
                memcpy(out.writeBuf, buf, _keep * sizeof(T));
                if (!out.swap(_keep)) { break; }
            }
            delete[] buf;
        }

        stream<T>* _in;
        int _outBlockSize;
        RingBuffer<T> ringBuf;
        std::thread bufferWorkerThread;
        std::thread workThread;
        int _keep, _skip;
        int _maxKeep = STREAM_BUFFER_SIZE;
    };
}