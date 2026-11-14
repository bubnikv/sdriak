#pragma once
#include <volk/volk.h>
#include <string.h>
#include <stdexcept>
#include <string>
#include "../../utils/flog.h"

namespace dsp::buffer {
    // volk_malloc() returns NULL on failure. Every caller here follows the
    // allocation with a memset/memcpy, so a silent NULL surfaces as a SIGSEGV
    // inside libc with nothing in the trace naming the allocation that failed.
    // Fail loudly instead: the log line and the exception message both carry
    // the requested size, and on Android an uncaught throw puts that message
    // in the tombstone's abort message.
    [[noreturn]] inline void throwAllocFailure(size_t bytes) {
        flog::error("dsp::buffer::alloc failed to allocate {0} bytes", (uint64_t)bytes);
        throw std::runtime_error("dsp::buffer::alloc: out of memory allocating " + std::to_string(bytes) + " bytes");
    }

    template<class T>
    inline T* alloc(int count) {
        const size_t bytes = (size_t)count * sizeof(T);
        T* buffer = (T*)volk_malloc(bytes, volk_get_alignment());
        if (!buffer) { throwAllocFailure(bytes); }
        return buffer;
    }

    template<class T>
    inline void clear(T* buffer, int count, int offset = 0) {
        memset(&buffer[offset], 0, count * sizeof(T));
    }

    inline void free(void* buffer) {
        volk_free(buffer);
    }

    // RAII owner for a volk-aligned scratch buffer.
    //
    // The point is exception safety on the OOM path. alloc() throws, and a
    // block's destructor bails on `if (!_block_init) { return; }` -- so an
    // init() that throws half-way used to leak everything it had allocated so
    // far, at exactly the moment memory is scarce. Member destructors run
    // regardless of that guard (and even when the owning constructor throws),
    // so a Buffer member is always released.
    //
    // The conversion to T* is implicit so a Buffer reads exactly like the raw
    // pointer it replaces at every process() call site. Use get() where a cast
    // is applied, since a C-style cast cannot see through a user conversion.
    template<class T>
    class Buffer {
    public:
        Buffer() {}
        explicit Buffer(int count) { allocate(count); }

        ~Buffer() { release(); }

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        Buffer(Buffer&& other) noexcept : _ptr(other._ptr), _size(other._size) {
            other._ptr = NULL;
            other._size = 0;
        }

        Buffer& operator=(Buffer&& other) noexcept {
            if (this != &other) {
                release();
                _ptr = other._ptr;
                _size = other._size;
                other._ptr = NULL;
                other._size = 0;
            }
            return *this;
        }

        // Strong guarantee: acquire first, commit second. Releasing up front
        // would leave the owner empty (and any derived pointer dangling) if the
        // replacement allocation throws -- which is exactly the OOM path this
        // class exists for.
        void allocate(int count) {
            T* fresh = dsp::buffer::alloc<T>(count);
            release();
            _ptr = fresh;
            _size = count;
        }

        void release() {
            if (!_ptr) { return; }
            dsp::buffer::free(_ptr);
            _ptr = NULL;
            _size = 0;
        }

        T* get() const { return _ptr; }
        int size() const { return _size; }

        operator T*() const { return _ptr; }

    private:
        T* _ptr = NULL;
        int _size = 0;
    };
}
