#pragma once
#include <string>
#include <utility>
#include <vector>

// A source that supplies the waterfall itself: a server-side spectrum
// computed by the remote receiver over a span wider than the IQ the link
// carries (KiwiSDR W/F, UberSDR, SpyServer FFT). Registered with
// IQFrontEnd::setSpectrumProvider(), which then stops the local FFT.
//
// All methods are called on the UI thread. GUI only: in --server mode no
// source registers a provider.
class SpectrumProvider {
public:
    virtual ~SpectrumProvider() = default;

    // Stable key, e.g. "KiwiSDR"; used for per-provider waterfall levels.
    virtual std::string name() const = 0;

    // Nominal bins per frame: the waterfall's raw line size. Frames of
    // another size are resampled onto it.
    virtual int binCount() const = 0;

    // Server zoom ladder: display spans in Hz, widest first. One entry
    // means "no server zoom" (the local view zoom stays active).
    virtual std::vector<double> zoomSpans() const = 0;

    // Ask for a view of span spanHz centred on the display centre. The
    // provider quantises, rate-limits and sends; it reports the span the
    // server confirms with core::setDisplaySpan().
    virtual void requestSpan(double spanHz) = 0;

    // True while the server spectrum is usable; false asks core to fall
    // back to the local FFT.
    virtual bool available() const = 0;

    // Why available() is false, for the source menu notice.
    virtual std::string unavailableReason() const { return std::string(); }

    // Waterfall levels {min, max} in the provider's own bin scale (dBm,
    // dBFS, server offset), used until the user sets their own. Levels
    // are kept per name().
    virtual std::pair<float, float> defaultLevels() const { return { -120.0f, -40.0f }; }
};
