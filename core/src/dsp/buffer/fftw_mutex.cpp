#include "fftw.h"

namespace dsp::buffer {
    // Plans can belong to static objects (notably sigpath::iqFrontEnd) that are
    // constructed before this function is first called. A destructible
    // function-local mutex would then be destroyed first, and those plan
    // destructors would lock a dead object during shutdown (an abort on libc++
    // and bionic). Give the lock process lifetime instead; the one allocation
    // is intentionally kept until the OS tears the process down.
    std::mutex& fftwMutex() {
        static std::mutex* const mtx = new std::mutex;
        return *mtx;
    }
}
