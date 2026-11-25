#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

// Server zoom: the zoom wheel, pinch and slider step through a provider's
// ladder of display spans (SpectrumProvider::zoomSpans(), widest first)
// instead of zooming the local view. Pure state, driven by the UI thread;
// the caller sends the spans it returns with requestSpan().
class ServerZoom {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kStepInterval{150};

    // Every UI tick. A changed ladder re-picks the rung nearest
    // currentSpan; an empty or one-rung ladder turns server zoom off.
    void setLadder(const std::vector<double>& spans, double currentSpan) {
        if (spans == m_spans) { return; }
        m_spans = spans;
        m_accum = 0.0f;
        m_rung = nearestRung(currentSpan);
    }

    bool active() const { return m_spans.size() > 1; }

    // Zoom wheel or pinch; positive zooms in (a narrower span).
    void wheel(float steps) {
        if (!active()) { return; }
        const float cap = (float)m_spans.size();
        m_accum = std::clamp(m_accum + steps, -cap, cap);
    }

    // Every UI tick: at most one rung per kStepInterval. Returns the span
    // to request, or 0.
    double tick(Clock::time_point now) {
        if (!active() || std::fabs(m_accum) < 1.0f) { return 0.0; }
        if (now - m_lastStep < kStepInterval) { return 0.0; }
        const int dir = (m_accum > 0.0f) ? 1 : -1;
        m_accum -= (float)dir;
        const int next = std::clamp(m_rung + dir, 0, (int)m_spans.size() - 1);
        if (next == m_rung) {
            m_accum = 0.0f;
            return 0.0;
        }
        m_rung = next;
        m_lastStep = now;
        return m_spans[(size_t)m_rung];
    }

    // Zoom slider position, 1 = widest rung, 0 = narrowest. Returns the
    // span to request when the rung changes, or 0.
    double slider(float pos) {
        if (!active()) { return 0.0; }
        const int last = (int)m_spans.size() - 1;
        const int rung = std::clamp((int)std::lround((1.0 - (double)pos) * last), 0, last);
        if (rung == m_rung) { return 0.0; }
        m_rung = rung;
        m_accum = 0.0f;
        return m_spans[(size_t)m_rung];
    }

    float sliderPos() const {
        if (!active()) { return 1.0f; }
        return 1.0f - ((float)m_rung / (float)(m_spans.size() - 1));
    }

    // The span the server confirmed (core::setDisplaySpan).
    void confirm(double span) {
        if (active()) { m_rung = nearestRung(span); }
    }

    int rung() const { return m_rung; }

private:
    int nearestRung(double span) const {
        int best = 0;
        for (int i = 1; i < (int)m_spans.size(); i++) {
            if (std::fabs(m_spans[(size_t)i] - span) < std::fabs(m_spans[(size_t)best] - span)) { best = i; }
        }
        return best;
    }

    std::vector<double> m_spans;
    int m_rung = 0;
    float m_accum = 0.0f;
    Clock::time_point m_lastStep{};
};
