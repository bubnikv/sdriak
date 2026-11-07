#pragma once
#include <string.h>
#include <assert.h>
#include "../types.h"
#include "../buffer/buffer.h"
// Only for the STREAM_BUFFER_SIZE default below. When block sizes become
// explicit (chunking), maxBlock becomes a required argument and this goes.
#include "../stream.h"

namespace dsp::math {
    // Computation kernel, not a dsp::block: no output stream, no worker thread,
    // no mutexes. Every use of Delay in this tree is inside a composite that
    // calls process() inline, so the threading machinery a Processor carries was
    // dead weight in all of them -- and each instance cost a 16 MB stream that
    // existed only to be freed again. The containing block owns the thread and
    // is responsible for stopping it before touching setDelay()/reset().
    template<class T>
    class Delay {
    public:
        Delay() {}

        Delay(int delay, int maxBlock = STREAM_BUFFER_SIZE) { init(delay, maxBlock); }

        void init(int delay, int maxBlock = STREAM_BUFFER_SIZE) {
            _delay = delay;
            _maxBlock = maxBlock;

            buffer.allocate(_maxBlock + 64000);
            bufStart = &buffer[_delay];
            buffer::clear<T>(buffer, _delay);

            _init = true;
        }

        void setDelay(int delay) {
            assert(_init);
            _delay = delay;
            bufStart = &buffer[_delay];
            reset();
        }

        void reset() {
            assert(_init);
            buffer::clear<T>(buffer, _delay);
        }

        inline int process(int count, const T* in, T* out) {
            // Copy data into delay buffer
            memcpy(bufStart, in, count * sizeof(T));

            // Copy data out of the delay buffer
            memcpy(out, buffer, count * sizeof(T));

            // Move end of the delay buffer to the front
            memmove(buffer, &buffer[count], _delay * sizeof(T));

            return count;
        }

    private:
        bool _init = false;
        int _delay = 0;
        int _maxBlock = 0;
        buffer::Buffer<T> buffer;
        T* bufStart = NULL;
    };
}
