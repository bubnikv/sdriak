#pragma once
#include <assert.h>
#include <string.h>
#include <algorithm>
#include <type_traits>
#include <vector>

namespace dsp::buffer {
    // Passive frame tap: cuts a sample stream into `keep`-sample frames, one
    // every `interval` input samples, and hands each frame to a callback.
    //
    // It owns no thread and no stream. The block that produces the samples
    // calls push() inline from its own run(), so the tap costs only bounded
    // history (keep for decimating taps, 2*keep otherwise) -- no extra worker
    // and no swap() that could stall the producer. This is what the diagnostic
    // displays (constellations, symbol scopes) and fixed integration windows
    // need.
    //
    // interval < keep gives a sliding window (each frame reuses keep-interval
    // samples of the previous one); interval > keep preserves Reshaper's old
    // decimating phase: capture the first keep samples of each interval, then
    // skip the remainder; interval == keep gives contiguous, non-overlapping
    // frames. Frame boundaries are sample-exact whatever block sizes push() is
    // called with.
    //
    // The callback runs on the producer's thread. A display callback must not
    // block -- try-lock the widget and drop the frame if the GUI holds it (see
    // ImGui::SymbolDiagram::tryWrite()).
    //
    // Not thread-safe: init() and reset() must not race push(), so call them
    // while the producing block is stopped.
    template <class T>
    class FrameTap {
        static_assert(std::is_trivially_copyable_v<T>, "FrameTap copies samples with memcpy");
    public:
        using Handler = void (*)(const T* frame, int count, void* ctx);

        FrameTap() {}
        FrameTap(int keep, int interval, Handler handler, void* ctx) { init(keep, interval, handler, ctx); }

        void init(int keep, int interval, Handler handler, void* ctx) {
            assert(keep > 0);
            assert(interval > 0);
            _keep = keep;
            _interval = interval;
            _handler = handler;
            _ctx = ctx;
            // Decimating taps only retain the published prefix. Sliding and
            // contiguous taps need 2*keep so their latest window is contiguous.
            _hist.assign((size_t)keep * ((interval > keep) ? 1 : 2), T{});
            reset();
        }

        // Forget the history; the next frame is published once `keep` new
        // samples have arrived and an interval has elapsed.
        void reset() {
            _pos = 0;
            _filled = 0;
            _since = 0;
        }

        // An uninitialised tap ignores push(), so a producer can call it
        // unconditionally.
        void push(const T* data, int count) {
            if (!_handler) { return; }
            if (_interval > _keep) {
                pushDecimating(data, count);
                return;
            }

            while (count > 0) {
                // Never step past the next frame boundary, so frames land on
                // exact sample positions regardless of the caller's block size.
                int n = (std::min)(count, _interval - _since);
                append(data, n);
                data += n;
                count -= n;
                _since += n;
                if (_since == _interval) {
                    _since = 0;
                    if (_filled >= _keep) { _handler(&_hist[_pos - _keep], _keep, _ctx); }
                }
            }
        }

        int keep() const { return _keep; }
        int interval() const { return _interval; }

    private:
        // Match Reshaper(keep, interval - keep): each cycle captures its first
        // keep samples, waits through the skipped tail, then publishes. Apart
        // from preserving the old phase, not copying the skipped tail is the
        // cheapest path for heavily decimated displays such as Falcon 9.
        void pushDecimating(const T* data, int count) {
            while (count > 0) {
                const int n = (std::min)(count, _interval - _since);
                if (_since < _keep) {
                    const int copy = (std::min)(n, _keep - _since);
                    memcpy(_hist.data() + _since, data, copy * sizeof(T));
                }

                data += n;
                count -= n;
                _since += n;
                if (_since == _interval) {
                    _since = 0;
                    _handler(_hist.data(), _keep, _ctx);
                }
            }
        }

        // Keeps the latest `keep` samples contiguous at [_pos - keep, _pos).
        // The buffer is 2*keep long, so the memmove that makes room runs at
        // most once per `keep` appended samples.
        void append(const T* data, int n) {
            if (n >= _keep) {
                memcpy(_hist.data(), data + (n - _keep), _keep * sizeof(T));
                _pos = _keep;
            }
            else {
                if (_pos + n > 2 * _keep) {
                    memmove(_hist.data(), _hist.data() + (_pos - _keep), _keep * sizeof(T));
                    _pos = _keep;
                }
                memcpy(_hist.data() + _pos, data, n * sizeof(T));
                _pos += n;
            }
            _filled = (std::min)(_filled + n, _keep);
        }

        int _keep = 0;
        int _interval = 0;
        Handler _handler = nullptr;
        void* _ctx = nullptr;

        std::vector<T> _hist;
        int _pos = 0;
        int _filled = 0;
        int _since = 0;
    };
}
