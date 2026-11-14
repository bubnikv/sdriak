#pragma once
#include <string.h>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <volk/volk.h>
#include "buffer/buffer.h"

// 1MSample buffer
#define STREAM_BUFFER_SIZE 1000000

namespace dsp {
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
        }

        virtual inline bool swap(int size) {
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
    };
}