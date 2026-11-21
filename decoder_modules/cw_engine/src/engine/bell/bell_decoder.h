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
 * (the .cxx files in src/ at commit 5eb011e, GPL-3.0-or-later), and
 * corrected against the listing. BELL.md has the routine-by-routine
 * comparison and what changed.
 *
 * The decoder takes one envelope-detector sample every 5 ms. Per sample it
 * extends each saved path (≤ 25) to 30 new nodes (6 element types × 5 speed
 * steps), runs one scalar Kalman filter on the signal amplitude per node,
 * scores the node by the filter's innovation, keeps the most probable nodes
 * and emits a letter once the saved paths share an ancestor (≤ 1 s delay).
 *
 * The routines keep Bell's names and argument order. Every index starts at
 * 0, one below the listing's: path ip, node j, letter state λ, element type
 * k, speed step i, trellis stage, and the table entries that hold one
 * (MEMFCN's letter states, ILAMI's element types, MEMPR's RTRANS columns).
 * The listing's 0 for "none" is −1 (kNone).
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
    // morse-wip: 5 and 80). Within a few WPM of the lower limit a character
    // space before a dit decodes as an element space (TEST as NST at 12–15
    // WPM with the listing's 10; BELL.md). Part of the reason: SPDTR gives
    // a step past a limit probability 0 without renormalising the others,
    // and after a character space RTRANS puts more on the outer steps than
    // after an element space. 5 keeps the limit below the speeds people send.
    int    wpm_min  = 5;
    int    wpm_max  = 60;
    // TRELIS: the longest decode delay, in 5 ms samples (NDELAY).
    int    ndelay   = 200;
    // MODEL: the process noise of the amplitude's random walk while the key
    // stays down, per 5 ms sample (listing: 0.0001, about 0.14 of a unit
    // amplitude per second). Fixed in the listing; under fading the right
    // value grows with the Doppler spread (BELL.md, "Fading").
    float  mark_q   = 0.0001f;
    // MODEL as a bank of filters (Magill): every node also carries one of
    // these values of the mark process noise, its q class, so the posterior
    // picks the fading rate as it picks the speed. A path keeps its class
    // while an element lasts and moves to another, each equally likely,
    // with total probability q_switch when the element changes. Empty: one
    // class, mark_q, and the listing's 30 nodes per path.
    std::vector<float> q_bank;
    float  q_switch = 0.05f;

    // Coherent variant (not in the listing): push_iq() takes complex
    // samples, every coh_dt_ms, scaled so the noise has variance 1. Each
    // node runs a linear Kalman filter on the channel's complex gain g,
    //   y = k·g + w,  g' = a·g + v,  v ~ CN(0, (1 − a²)·Ω),
    // which is exact given the node's key sequence: in a mark it updates on
    // y, in a space it only predicts. a comes from the Doppler spread B
    // (the bench's B = 2σ_f): an AR(1) whose correlation equals the
    // Gaussian exp(−π²B²τ²/2) at τ = coh_match_ms. Ω, the mean signal
    // power, is learned from the marks. NOISE, MODEL and the clamps are not
    // used. coh_spreads, when not empty, makes the spread a hidden class per
    // path, like q_bank (and q_switch).
    bool   coherent      = false;
    float  coh_dt_ms     = 8.0f;
    float  coh_spread    = 0.5f;
    std::vector<float> coh_spreads;
    float  coh_match_ms  = 40.0f;
    float  coh_omega0    = 10.0f;     // Ω before any mark, per sample
    // Impulses: the noise is w ~ (1 − coh_imp_eps)·CN(0, 1) +
    // coh_imp_eps·CN(0, coh_imp_kappa). A sample far outside the Gaussian
    // part then counts as weak evidence for either key state instead of
    // strong evidence for a mark, and moves the filter by its posterior
    // weight of not being an impulse. 0: Gaussian noise.
    float  coh_imp_eps   = 0.0f;
    float  coh_imp_kappa = 1000.0f;
    // Weight the filter update by that posterior too. Off: the mixture
    // only scores, and the filter always takes the Gaussian update (a fast
    // fade then still updates the gain).
    bool   coh_imp_update = true;
    // Weigh samples an impulse repair stage rebuilt (push_iq's w) at 1 − w
    // of their evidence.
    bool   coh_repair_weight = false;

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

    // morse-wip's code tree, for A/B runs: a code that runs off the tree
    // goes on from the root, so its tail decodes as a new character (the
    // 8-dit error sign as "I"). Off, such a code stays in a sink node and
    // prints one '#'.
    bool port_tree = false;

    // The listing alone: no clamp, 10–60 WPM.
    static BellConfig listing();
    // morse-wip as published: 20 paths, 5–80 WPM, the sign error, its code
    // tree, the clamp.
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
    // The coherent variant: one complex sample every coh_dt_ms, noise
    // variance 1.
    // w: the share of the sample an impulse repair rebuilt (used with
    // BellConfig::coh_repair_weight).
    void push_iq(float re, float im, std::string &out, float w = 0.0f);

    // State of the last sample.
    float key_prob() const { return m_px; }             // PX: P(key down)
    int   key_state() const { return m_xhat; }          // XHAT
    // ELMHAT: 0 dit, 1 dah, 2 element space, 3 character space, 4 word
    // space, 5 pause.
    int   element() const { return m_elmhat; }
    float speed_wpm() const { return m_spdhat; }        // SPDHAT
    float noise_power() const { return m_rn; }          // RN
    float best_prob() const { return m_p[m_imax]; }     // P(IMAX)
    float amplitude() const;                            // YKKIP(IMAX); coherent: |g|
    float signal_power() const { return m_omega; }      // coherent: Ω
    int   saved_paths() const { return m_isave; }       // ISAVE
    // The posterior mean of the mark process noise over the q classes.
    float mark_q_est() const { return m_qhat; }
    // Coherent, one spread class: the spread, measured on the station
    // (BellBackendConfig::measure_spread), in place of coh_spread.
    void  set_spread(float b);

private:
    // Element types k.
    static constexpr int kDit = 0, kDah = 1, kElemSpace = 2, kCharSpace = 3, kWordSpace = 4,
                         kPause = 5;
    static constexpr int kElems    = 6;                 // element types
    static constexpr int kRates    = 5;                 // speed steps i: −2..+2 steps
    static constexpr int kRateHold = 2;                 // the speed step that keeps the speed
    static constexpr int kNodes    = kElems * kRates;   // new nodes per path and q class
    static constexpr int kStates   = 16;                // letter states λ
    // No letter state: a transition the model does not allow, a pruned path.
    static constexpr int kNone     = -1;
    static constexpr int kLong     = 200;               // NOISE's windows, samples
    static constexpr int kShort    = 50;

    // PROCES, KALFIL, TRELIS and TRANSL back to their DATA state; NOISE
    // keeps its own.
    void   restart();

    // Bell's subroutines, in the listing's order.
    void   noise(float zin, float &rn, float &z);
    void   proces(float z, float rn, std::string &out);
    void   trprob(int ip, int lambda, float dur, int ilrate);
    double xtrans(int ielem, float d0, int irate) const;
    void   ptrans(int kelem, int irate, int lambda, int ilrate, float ptrx, float &psum,
                  float *pin, int n, float qstay, float qmove) const;
    double spdtr(int isrt, int ilrt, int iselm, int ilelm) const;
    void   path(int ip, int lambda, float dur, int ilrate);
    void   likhd(float z, float rn, int ip, int lambda, float dur, int ilrate);
    float  kalfil(float z, int ip, float rn, int ixs, int kelem, int jnode, float dur,
                  int ilrate, float pin, float qmark);
    // The coherent KALFIL: the log-likelihood of the sample (less a constant
    // common to all nodes), kNoLog when the node is not scored.
    float  kalfil_coh(int ip, int ixs, int jnode, float pin, int c);
    void   model(float dur, int ielm, int ilr, int ixs, float qmark, float &phi, float &qa) const;
    bool   probp();
    void   sprob();
    void   savep();
    void   trelis(std::string &out);
    void   transl(int ltr, std::string &out);

    // The listing's two-dimensional arrays: PIN(IP, N) per saved path and
    // new node, PTHTRL(N, I) and LMDSAV(N, I) per trellis stage and path.
    float &pin(int ip, int n) { return m_pin[ip * m_nodes + n]; }
    int   &pthtrl(int n, int i) { return m_pthtrl[n * m_cfg.paths + i]; }
    int   &lmdsav(int n, int i) { return m_lmdsav[n * m_cfg.paths + i]; }

    BellConfig m_cfg;

    // The q classes: their mark process noise, how many, and the new nodes
    // per saved path (30 per class).
    std::vector<float> m_qval;
    int m_nq = 1, m_nodes = kNodes;
    // The sample period, ms (5; coherent: coh_dt_ms).
    float m_dt = kSampleMs;

    // Coherent: the sample, the AR(1) coefficient of each class, Ω, and the
    // imaginary part of each filter's mean (the real part is YKKIP/YKKSV).
    static constexpr float kNoLog = -1e30f;
    float m_yr = 0.0f, m_yi = 0.0f;
    float m_wt = 0.0f;                      // the sample's repair weight
    std::vector<float> m_coha;
    float m_omega = 10.0f;
    std::vector<float> m_yikip, m_yiksv;
    // The impulse mixture's constants: ln(1 − ε), ln ε, ln κ.
    float m_ln_gauss = 0.0f, m_ln_imp = 0.0f, m_ln_kappa = 0.0f;

    // PROCES: the saved paths ip < ISAVE, and their new nodes
    // j = m_nodes·ip + n, n = 30·c + 6·i + k for q class c.
    int m_isave = 0;
    std::vector<int>   m_qcls, m_qcsav;         // q class of each path, node
    std::vector<float> m_pint;                  // TRPROB's scratch
    std::vector<int>   m_lambda, m_ilrate, m_pathsv, m_sort;
    std::vector<float> m_dur;
    std::vector<float> m_p;                     // P: saved paths, then new nodes
    std::vector<int>   m_lamsav, m_ilrsav;
    std::vector<float> m_dursav, m_lkhd;
    std::vector<float> m_pin;                   // PIN(IP, N)
    // Scratch of PROBP and SAVEP: their PSAV arrays, the nodes SAVEP has
    // kept so far, its merge flags ICONV.
    std::vector<float> m_psav_node, m_psav_path;
    std::vector<char>  m_saved, m_iconv;
    int   m_imax = 0;
    int   m_xhat = 0;
    int   m_elmhat = kDit;
    float m_px = 0.0f, m_spdhat = 0.0f, m_pelm = 0.0f, m_qhat = 0.0f;
    float m_rn = 0.1f;

    // KALFIL: the amplitude filter of each saved path and of each new node.
    std::vector<float> m_ykkip, m_pkkip, m_ykksv, m_pkksv;

    // NOISE: the newest positions KL, KS in the two rings, and how much of
    // each ring the minimum is taken over, KKL and KKS.
    float m_ylong[kLong] = {}, m_yshort[kShort] = {};
    int   m_kl = 0, m_kkl = 1, m_ks = 0, m_kks = 1;
    float m_ymin1 = 1.0f, m_ymin2 = 1.0f, m_ymavg = 0.05f;

    // TRELIS: the newest stage N in the ring of NDELAY stages, and NDELST,
    // the delay of the last meeting point in stages; both −1 before the
    // first sample.
    std::vector<int> m_pthtrl, m_lmdsav, m_ipnod, m_ltrsv;
    int m_n = -1, m_ndelst = -1;

    // TRANSL (morse-wip's: the 16-state model emits elements, a code tree
    // turns them into characters).
    int m_ixlast = 0, m_curstate = 0, m_newstate = 0;
};
