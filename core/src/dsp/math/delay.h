#pragma once
#include <string.h>
#include <assert.h>
#include "../types.h"
#include "../buffer/history_buffer.h"
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

        // The delay line is a HistoryBuffer whose history is the delay: the
        // output block is the first count samples of the window.
        void init(int delay, int maxBlock = STREAM_BUFFER_SIZE) {
            hist.init(maxBlock, delay);
            _init = true;
        }

        // Clears the delay line. Reallocates only when the new delay no
        // longer fits; a throw leaves the old delay in place.
        void setDelay(int delay) {
            assert(_init);
            hist.resize(hist.maxBlock(), delay);
        }

        void reset() {
            assert(_init);
            hist.clear();
        }

        // IN-PLACE: safe (in == out).
        inline int process(int count, const T* in, T* out) {
            const T* window = hist.push(in, count);
            memcpy(out, window, count * sizeof(T));
            hist.advance(count);

            return count;
        }

    private:
        bool _init = false;
        buffer::HistoryBuffer<T> hist;
    };
}
