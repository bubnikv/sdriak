#pragma once
#include "block.h"

// These macros define a run() function using a specic expression for processing
// This is needed because not all process functions have the same arguments

#define OVERRIDE_PROC_RUN(exp)\
    int run() {\
        int count = _in->read();\
        if (count < 0) {\
            return -1;\
        }\
        \
        exp;\
        \
        base_type::_in->flush();\
        if (!base_type::out.swap(count)) { return -1; }\
        return count;\
    }

#define OVERRIDE_MULTIRATE_PROC_RUN(exp)\
    int run() {\
        int count = _in->read();\
        if (count < 0) {\
            return -1;\
        }\
        \
        int outCount = exp;\
        \
        base_type::_in->flush();\
        if (outCount) {\
            if (!base_type::out.swap(outCount)) { return -1; }\
        }\
        return count;\
    }

#define DEFAULT_PROC_RUN            OVERRIDE_PROC_RUN(process(count, base_type::_in->readBuf, base_type::out.writeBuf))
#define DEFAULT_MULTIRATE_PROC_RUN  OVERRIDE_MULTIRATE_PROC_RUN(process(count, base_type::_in->readBuf, base_type::out.writeBuf))

namespace dsp {
    template <class I, class O>
    class Processor : public block {
    public:
        Processor() {}

        Processor(stream<I>* in) { init(in); }

        // `out` below is a member of *this* class, so it is destroyed before
        // ~block() gets to join the worker -- which would then be writing into
        // freed stream buffers. Stopping here, in the body of the destructor
        // that owns the stream, is the only placement that covers every
        // subclass; the per-class `~X() { stop(); }` overrides remain necessary
        // for classes that own buffers in their *own* subobject, and are cheap
        // no-ops once they have run (stop() returns immediately if !running).
        //
        // Qualified block::stop() rather than the virtual call: dispatch here
        // resolves to the base anyway, and saying so documents that a subclass
        // doStop() override would not run at this point.
        virtual ~Processor() {
            if (_block_init) { block::stop(); }
        }

        virtual void init(stream<I>* in) {
            _in = in;
            registerInput(_in);
            registerOutput(&out);
            _block_init = true;
        }

        virtual void setInput(stream<I>* in) {
            assert(_block_init);
            assert(in); // there is no detached state: run() dereferences _in unconditionally
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            dsp::block::TempStop guard(this);
            unregisterInput(_in);
            _in = in;
            registerInput(_in);
        }

        virtual int run() = 0;

        stream<O> out;

    protected:
        stream<I>* _in;
    };
}
