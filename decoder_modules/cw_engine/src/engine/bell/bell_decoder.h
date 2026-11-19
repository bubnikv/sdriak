/*
 * bell_decoder.h — E. L. Bell's Bayesian manual-Morse decoder (NPS, 1977).
 *
 * E. L. Bell, "Optimal Bayesian estimation of the state of a probabilistically
 * mapped memory-conditional Markov process with application to manual Morse
 * decoding", Doctor of Engineering thesis, Naval Postgraduate School, 1977
 * (DTIC ADA046503; Calhoun hdl 10945/18334). The thesis's Fortran listing,
 * pages 167–190, is the reference for every routine here.
 *
 * Translated from M. Niininen (AG1LE)'s C port, github.com/ag1le/morse-wip
 * (src/*.cxx at commit 5eb011e, GPL-3.0-or-later), and corrected against the
 * listing. BELL.md has the routine-by-routine comparison and what changed.
 *
 * The decoder takes one envelope-detector sample every 5 ms. Per sample it
 * extends each saved path (≤ 25) to 30 new nodes (6 element types × 5 speed
 * steps), runs one scalar Kalman filter on the signal amplitude per node,
 * scores the node by the filter's innovation, keeps the most probable nodes
 * and emits a letter once the saved paths share an ancestor (≤ 1 s delay).
 *
 * The routines keep Bell's names, argument order and 1-based indices (path
 * ip, node j, letter state λ, element k, speed step i), so each one reads
 * side by side with the listing.
 *
 * Copyright (C) 1977 E. L. Bell (U.S. Government work)
 * Copyright (C) 2012-2014 Mauri Niininen, AG1LE
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include <string>
#include <vector>

struct BellConfig {
    // SAVEP: the most paths saved (listing: 25; morse-wip: PATHS = 20).
    // At least 7: one per element type and one more.
    int    paths    = 25;
    // SAVEP: save paths until this much posterior mass is kept (POPT).
    double popt     = 0.99;
    // SPDTR: speeds a path may move to, WPM (listing: 10 and 60;
    // morse-wip: 5 and 80).
    int    wpm_min  = 10;
    int    wpm_max  = 60;
    // TRELIS: the longest decode delay, in 5 ms samples (NDELAY).
    int    ndelay   = 200;

    // Clamp NOISE's output to [0, 1] before PROCES (morse-wip, bmorse.cxx:352;
    // not in the listing). NOISE's variance estimate is fitted to Bell's
    // receiver; on any other front end a noise sample far below zero is
    // better explained as a mark of uncertain amplitude than as a space of
    // variance RN, and decodes as a character. The clamp turns it into a
    // clean space: on the bench it removes most characters decoded from the
    // noise before an over (BELL.md, "Measurements").
    bool clamp_z = true;

    // morse-wip's sign error, for A/B runs only: xtrans.cxx:78 computes the
    // survival past the nominal duration as -0.5·exp(...) instead of
    // 0.5·exp(...), so no element may outlast its nominal length unless the
    // 5 ms grid lands exactly on it.
    bool port_xtrans_sign = false;

    // The listing alone: no clamp.
    static BellConfig listing();
    // morse-wip as published: 20 paths, 5–80 WPM, the sign error, the clamp.
    static BellConfig port();
};

class BellDecoder {
public:
    static constexpr float kSampleMs = 5.0f;     // one sample per 5 ms (200 Hz)
    static constexpr double kRateHz  = 200.0;

    explicit BellDecoder(const BellConfig &cfg = BellConfig());

    // Back to the listing's DATA-statement state.
    void reset();

    // One detected-envelope sample (the input of Bell's NOISE). Decoded
    // characters are appended to out.
    void push(float zin, std::string &out);

    // State of the last sample.
    float key_prob() const { return m_px; }             // PX: P(key down)
    int   key_state() const { return m_xhat; }          // XHAT
    int   element() const { return m_elmhat; }          // ELMHAT, 1..6
    float speed_wpm() const { return m_spdhat; }        // SPDHAT
    float noise_power() const { return m_rn; }          // RN
    float best_prob() const;                            // P(IMAX)
    float amplitude() const;                            // YKKIP(IMAX)
    int   saved_paths() const { return m_isave; }       // ISAVE

private:
    // Bell's subroutines, in the listing's order.
    void   noise(float zin, float &rn, float &z);
    void   proces(float z, float rn, std::string &out);
    void   trprob(int ip, int lambda, float dur, int ilrate);
    double xtrans(int ielem, float d0, int irate) const;
    void   ptrans(int kelem, int irate, int lambda, int ilrate, float ptrx, float &psum,
                  float *pin, int n) const;
    double spdtr(int isrt, int ilrt, int iselm, int ilelm) const;
    void   path(int ip, int lambda, float dur, int ilrate);
    void   likhd(float z, float rn, int ip, int lambda, float dur, int ilrate);
    float  kalfil(float z, int ip, float rn, int ixs, int kelem, int jnode, float dur,
                  int ilrate, float pin);
    void   model(float dur, int ielm, int ilr, int ixs, float &phi, float &qa) const;
    bool   probp();
    void   sprob();
    void   savep();
    void   trelis(std::string &out);
    void   transl(int ltr, std::string &out);

    // 1-based views of the per-path and per-node arrays.
    float &pin(int ip, int n) { return m_pin[ip * 31 + n]; }
    int   &pthtrl(int n, int i) { return m_pthtrl[n * (m_cfg.paths + 1) + i]; }
    int   &lmdsav(int n, int i) { return m_lmdsav[n * (m_cfg.paths + 1) + i]; }

    BellConfig m_cfg;

    // PROCES: the saved paths (1..ISAVE) and the new nodes (1..30·ISAVE).
    int m_isave = 0;
    std::vector<int>   m_lambda, m_ilrate, m_pathsv, m_sort;
    std::vector<float> m_dur;
    std::vector<float> m_p;                     // P: saved paths, then new nodes
    std::vector<int>   m_lamsav, m_ilrsav;
    std::vector<float> m_dursav, m_lkhd;
    std::vector<float> m_pin;                   // PIN(IP, N)
    int   m_imax = 1;
    int   m_xhat = 0;
    int   m_elmhat = 1;
    float m_px = 0.0f, m_spdhat = 0.0f, m_pelm = 0.0f;
    float m_rn = 0.1f;

    // KALFIL: the amplitude filter of each saved path and of each new node.
    std::vector<float> m_ykkip, m_pkkip, m_ykksv, m_pkksv;

    // NOISE.
    float m_ylong[201] = {}, m_yshort[51] = {};
    int   m_kl = 1, m_kkl = 1, m_ks = 1, m_kks = 1;
    float m_ymin1 = 1.0f, m_ymin2 = 1.0f, m_ymavg = 0.05f;

    // TRELIS.
    std::vector<int> m_pthtrl, m_lmdsav, m_ipnod, m_ltrsv;
    int m_n = 0, m_ndelst = 0;

    // TRANSL (morse-wip's: the 16-state model emits elements, a code tree
    // turns them into characters).
    int m_ixlast = 0, m_curstate = 0, m_newstate = 0;
};
