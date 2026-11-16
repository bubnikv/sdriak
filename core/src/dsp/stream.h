#pragma once
#include <string.h>
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <volk/volk.h>
#include "buffer/buffer.h"
#include "../utils/flog.h"

// Default capacity of every dsp::stream, in elements, and the implicit block
// bound of everything derived from it. 32-bit Android uses 262144: the largest
// block any source loaded there writes is HackRF's 131072 (2x margin), and the
// fixed 48 kHz audio sink never needs more than a few thousand samples per
// block. Everything else keeps 1000000. Remote sources whose block size is set
// by the server (SpyServer, the SDRIAK server client) split their output to
// the stream's capacity() rather than assuming this value.
// see doc/design/buffer-size-requirements-current.md
#if defined(__ANDROID__) && !defined(__LP64__)
#define STREAM_BUFFER_SIZE 262144
#else
#define STREAM_BUFFER_SIZE 1000000
#endif

namespace dsp {
    // A swap() larger than the buffer means the producer has already written
    // past the end of it: the heap is corrupt. Stop here, with a message naming
    // the sizes, instead of continuing into a crash somewhere unrelated.
    [[noreturn]] inline void streamOverflow(int size, int capacity) {
        flog::error("dsp::stream overflow: swap({0}) on a stream of capacity {1}", size, capacity);
        std::abort();
    }

    class untyped_stream {
    public:
        virtual ~untyped_stream() {}
        virtual bool swap(int size) { return false; }
        virtual int read() { return -1; }
        virtual void flush() {}
        virtual void stopWriter() {}
        virtual void clearWriteStop() {}
        virtual void stopReader() {}
        virtual void clearReadStop() {}
    };

    template <class T>
    class stream : public untyped_stream {
    public:
        stream() {
            // alloc() throws on OOM. Release the first buffer if the second
            // fails, otherwise it leaks: a throwing constructor gets no
            // destructor call, and we are already at the memory ceiling here.
            writeBuf = buffer::alloc<T>(STREAM_BUFFER_SIZE);
            try {
                readBuf = buffer::alloc<T>(STREAM_BUFFER_SIZE);
            }
            catch (...) {
                buffer::free(writeBuf);
                throw;
            }
            _capacity = STREAM_BUFFER_SIZE;
        }

        virtual ~stream() {
            free();
        }

        virtual void setBufferSize(int samples) {
            // Allocate both replacements before releasing anything, so a
            // failure leaves the stream on its existing buffers rather than
            // empty or half-sized. Costs a transient 2x, which is acceptable:
            // this is only called once at init time, never on a hot path.
            T* newWrite = buffer::alloc<T>(samples);
            T* newRead = NULL;
            try {
                newRead = buffer::alloc<T>(samples);
            }
            catch (...) {
                buffer::free(newWrite);
                throw;
            }
            buffer::free(writeBuf);
            buffer::free(readBuf);
            writeBuf = newWrite;
            readBuf = newRead;
            _capacity = samples;
        }

        // Elements each of writeBuf and readBuf holds. A producer whose block
        // size is not structurally bounded (a network peer, a library callback)
        // must split its output to this, not to STREAM_BUFFER_SIZE: streams
        // can be resized.
        int capacity() const { return _capacity; }

        virtual inline bool swap(int size) {
            // Secondary detection only: the write has already happened. It
            // turns silent heap corruption into an immediate, named failure.
            if (size < 0 || size > _capacity) { streamOverflow(size, _capacity); }
            {
                // Wait to either swap or stop
                std::unique_lock<std::mutex> lck(swapMtx);
                swapCV.wait(lck, [this] { return (canSwap || writerStop); });

                // If writer was stopped, abandon operation
                if (writerStop) { return false; }

                // Swap buffers
                dataSize = size;
                T* temp = writeBuf;
                writeBuf = readBuf;
                readBuf = temp;
                canSwap = false;
            }

            // Notify reader that some data is ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = true;
            }
            rdyCV.notify_all();

            return true;
        }

        virtual inline int read() {
            // Wait for data to be ready or to be stopped
            std::unique_lock<std::mutex> lck(rdyMtx);
            rdyCV.wait(lck, [this] { return (dataReady || readerStop); });

            return (readerStop ? -1 : dataSize);
        }

        // Like read(), but returns 0 if no data became ready within `timeout`.
        // -1 still means stopReader() was called. Used by realtime audio sink
        // callbacks that must not block past one device period.
        virtual inline int read_for(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lck(rdyMtx);
            if (!rdyCV.wait_for(lck, timeout, [this] { return (dataReady || readerStop); })) {
                return 0;
            }
            return (readerStop ? -1 : dataSize);
        }

        virtual inline void flush() {
            // Clear data ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = false;
            }

            // Notify writer that buffers can be swapped
            {
                std::lock_guard<std::mutex> lck(swapMtx);
                canSwap = true;
            }

            swapCV.notify_all();
        }

        virtual void stopWriter() {
            {
                std::lock_guard<std::mutex> lck(swapMtx);
                writerStop = true;
            }
            swapCV.notify_all();
        }

        virtual void clearWriteStop() {
            writerStop = false;
        }

        virtual void stopReader() {
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                readerStop = true;
            }
            rdyCV.notify_all();
        }

        virtual void clearReadStop() {
            readerStop = false;
        }

        void free() {
            if (writeBuf) { buffer::free(writeBuf); }
            if (readBuf) { buffer::free(readBuf); }
            writeBuf = NULL;
            readBuf = NULL;
            _capacity = 0;
        }

        T* writeBuf;
        T* readBuf;

    private:
        std::mutex swapMtx;
        std::condition_variable swapCV;
        bool canSwap = true;

        std::mutex rdyMtx;
        std::condition_variable rdyCV;
        bool dataReady = false;

        bool readerStop = false;
        bool writerStop = false;

        int dataSize = 0;
        int _capacity = 0;
    };

    // Block size for a source that delivers samplerate/divisor samples per
    // call (sr/200 is 5 ms), bounded by what the destination stream actually
    // holds. Use it instead of a bare sr/200: drivers such as UHD, SoapySDR
    // and LMS treat the count as the caller's buffer capacity, so an unbounded
    // rate becomes a heap overflow. Clamped in double so a huge rate cannot
    // overflow the int.
    template <class T>
    inline int sourceBlockSize(double samplerate, const stream<T>& out, int divisor = 200) {
        const double block = samplerate / (double)divisor;
        if (!(block >= 1.0)) { return 1; }
        return (block >= (double)out.capacity()) ? out.capacity() : (int)block;
    }
}