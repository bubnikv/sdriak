/*
 * cw_rundump.h — the run dump (SKIM_CW_DUMP_RUNS, an offline ML corpus).
 *
 * An independent v1-style Schmitt keyer with the bootstrap's 3-sample
 * glitch fold, running on every sample regardless of gate/pause state —
 * diagnostics only: the decode path never reads any of this. One line per
 * completed run into <SKIM_CW_DUMP_RUNS>: "freq_hz t_ms key dur_ms" (C
 * locale); and the decode's own text, char by char, into
 * <SKIM_CW_DUMP_RUNS>.chars: "freq_hz t_ms ascii timing_error solid". A char's
 * time is the end of its AUDIO (the same sample clock as the runs), so it
 * aligns to the run stream exactly; timing_error/solid carry v2's own quality
 * judgment at the commit. The offline harvest labels its chunks with these
 * instead of a trivial re-decode — v2's squelch and lattice are the label's
 * defenses.
 *
 * SDRIAK: the two files are a CwRunDumpSink, created by whoever owns the
 * decoders and shared by every channel that writes them through
 * CwDiagConfig::dump (null = off) — upstream: process-wide files the first
 * channel opens; rd_file(), rd_emit(), rd_char_file(), rd_emit_char() are
 * the sink's methods. CwRunDump is a channel's keyer on the sink (upstream:
 * the rd_on/rd_key/rd_run/rd_pend_* fields of the decoder state).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"
#include "cw_schmitt.h"
#include "decode_cw_config.h"

// The dump's two files. Each opens at its first write (once: a file that
// cannot be created stays off); writes from any thread are serialised.
class CwRunDumpSink {
public:
    // The sink SKIM_CW_DUMP_RUNS names; null when it is unset. Call it once and
    // share the result: two sinks on one path write over each other.
    static std::shared_ptr<CwRunDumpSink> from_env() {
        const char *e = std::getenv("SKIM_CW_DUMP_RUNS");
        return e ? std::make_shared<CwRunDumpSink>(e) : nullptr;
    }

    explicit CwRunDumpSink(std::string path) : m_path(std::move(path)) {}
    ~CwRunDumpSink() {
        if (m_runs) { fclose(m_runs); }
        if (m_chars) { fclose(m_chars); }
    }
    CwRunDumpSink(const CwRunDumpSink &) = delete;
    CwRunDumpSink &operator=(const CwRunDumpSink &) = delete;

    // One completed run of the channel at freq_hz: key down or up, len
    // samples, ending at sample t (rate).
    void run(double freq_hz, uint64_t t, double rate, bool key, double len) {
        char hz[40];
        char tms[40];
        char dur[40];
        std::snprintf(hz, sizeof(hz), "%.1f", freq_hz);
        std::snprintf(tms, sizeof(tms), "%.1f", static_cast<double>(t) * 1000.0 / rate);
        std::snprintf(dur, sizeof(dur), "%.1f", len * 1000.0 / rate);
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (FILE *f = open(m_runs, m_runs_tried, m_path)) {
            fprintf(f, "%s %s %d %s\n", hz, tms, key ? 1 : 0, dur);
        }
    }

    // One emitted char whose audio ended at t_end, with the clock's quality.
    void emit_char(double freq_hz, uint64_t t_end, double rate, char c, double timing_error,
                   bool solid) {
        char hz[40];
        char tms[40];
        char er[40];
        std::snprintf(hz, sizeof(hz), "%.1f", freq_hz);
        std::snprintf(tms, sizeof(tms), "%.1f", static_cast<double>(t_end) * 1000.0 / rate);
        std::snprintf(er, sizeof(er), "%.3f", timing_error);
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (FILE *f = open(m_chars, m_chars_tried, m_path + ".chars")) {
            fprintf(f, "%s %s %d %s %d\n", hz, tms, static_cast<int>(static_cast<unsigned char>(c)), er,
                    solid ? 1 : 0);
        }
    }

private:
    static FILE *open(FILE *&f, bool &tried, const std::string &path) {
        if (!tried) {
            tried = true;
            f = fopen(path.c_str(), "w");
        }
        return f;
    }

    std::mutex  m_mutex;
    std::string m_path;                   // SKIM_CW_DUMP_RUNS
    FILE       *m_runs = nullptr;         // <path>
    FILE       *m_chars = nullptr;        // <path>.chars
    bool        m_runs_tried = false, m_chars_tried = false;
};

// A channel's run keyer, dumping into the sink.
class CwRunDump {
public:
    void init(const CwDiagConfig &cfg) { m_sink = cfg.dump; }

    bool on() const { return m_sink != nullptr; }

    // One sample m of the run keyer against the levels lo/hi (sample t,
    // rate): a confirmed run goes into the dump.
    void step(double m, double lo, double hi, double freq_hz, uint64_t t, double rate) {
        CwSchmitt::Run run;
        if (m_keyer.step(m, lo, hi, run)) {
            m_sink->run(freq_hz, t, rate, run.key, run.len);
            m_keyer.settle();
        }
    }

    // One emitted char whose audio ended at t_end.
    void emit_char(double freq_hz, uint64_t t_end, double rate, char c, double timing_error,
                   bool solid) {
        m_sink->emit_char(freq_hz, t_end, rate, c, timing_error, solid);
    }

private:
    std::shared_ptr<CwRunDumpSink> m_sink;   // null = off
    CwSchmitt m_keyer;                       // the run keyer
};
