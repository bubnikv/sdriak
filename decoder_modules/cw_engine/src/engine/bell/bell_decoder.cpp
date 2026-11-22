/*
 * bell_decoder.cpp — E. L. Bell's Bayesian manual-Morse decoder (see
 * bell_decoder.h). Each routine carries the listing's name and page, and
 * notes where it departs from the listing or from morse-wip.
 *
 * Copyright (C) 1977 E. L. Bell (U.S. Government work)
 * Copyright (C) 2012-2014 Mauri Niininen, AG1LE
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "bell_decoder.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

// --- INITL's tables (listing p. 160–161) ------------------------------------
//
// This is the listing's 16-state model: a letter state λ is the pair
// (previous element, current element) — 0..7 a space after a mark, 8..15 a
// mark after a space. Bell's "independent letter" tree model reads 300 more
// states from a file, MORSEM, that the listing does not contain; morse-wip,
// and this file, run the 16-state model.
//
// Element types k: 0 dit, 1 dah, 2 element space, 3 character space, 4 word
// space, 5 pause. Speed steps i: 0..4 = −2..+2 steps. An entry that is an
// index is the listing's minus 1, so the listing's 0, "none", is −1.

// ISX(K): the key state of element type K. The listing's ILAMX(E), the key
// state of element type E, is the same table.
constexpr int kIsx[6] = {1, 1, 0, 0, 0, 0};
// ILAMI(L): the element type of letter state L (IELMST is the identity here).
constexpr int kIlami[16] = {2, 3, 4, 5, 2, 3, 4, 5, 0, 1, 0, 1, 0, 1, 0, 1};
// MEMFCN(L, K): the letter state after state L when element K follows; −1 =
// not allowed. [K][L].
constexpr int kMemfcn[6][16] = {
    {8, 10, 12, 14, 8, 10, 12, 14, 8, -1, 10, -1, 12, -1, 14, -1},
    {9, 11, 13, 15, 9, 11, 13, 15, -1, 9, -1, 11, -1, 13, -1, 15},
    {0, -1, -1, -1, 4, -1, -1, -1, 0, 4, 0, 4, 0, 4, 0, 4},
    {-1, 1, -1, -1, -1, 5, -1, -1, 1, 5, 1, 5, 1, 5, 1, 5},
    {-1, -1, 2, -1, -1, -1, 6, -1, 2, 6, 2, 6, 2, 6, 2, 6},
    {-1, -1, -1, 3, -1, -1, -1, 7, 3, 7, 3, 7, 3, 7, 3, 7},
};
// ELEMTR(L, K): P(next element K | letter state L), Bell's Table XII (the
// second-order symbol model). [K][L].
constexpr float kElemtr[6][16] = {
    {.55f, .5f, .5f, .5f, .55f, .5f, .5f, .5f, 0, 0, 0, 0, 0, 0, 0, 0},
    {.45f, .5f, .5f, .5f, .45f, .5f, .5f, .5f, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, .581f, .54f, .923f, .923f, .923f, .923f, .95f, .95f},
    {0, 0, 0, 0, 0, 0, 0, 0, .335f, .376f, .062f, .062f, .062f, .062f, .04f, .04f},
    {0, 0, 0, 0, 0, 0, 0, 0, .069f, .069f, .012f, .012f, .012f, .012f, .009f, .009f},
    {0, 0, 0, 0, 0, 0, 0, 0, .015f, .015f, .003f, .003f, .003f, .003f, .001f, .001f},
};
// RTRANS(I, IND): P(speed step I), IND 0 after a mark, an element space or
// a word space, 1 after a character space or a pause. [IND][I].
constexpr float kRtrans[2][5] = {
    {.1f, .2f, .4f, .2f, .1f},
    {.15f, .2f, .3f, .2f, .15f},
};
// MEMDEL(ILELM, ISELM): WPM per speed step from element ILELM to ISELM.
// [ISELM][ILELM], the DATA statement's column order.
constexpr int kMemdel[6][6] = {
    {0, 0, 2, 2, 5, 10},
    {0, 0, 2, 2, 5, 10},
    {2, 2, 0, 0, 0, 0},
    {2, 2, 0, 0, 0, 0},
    {2, 2, 0, 0, 0, 0},
    {2, 2, 0, 0, 0, 0},
};
// MEMPR(ILELM, ISELM): which RTRANS column applies; −1 between two marks or
// two spaces, where neither element can follow the other (SPDTR settles an
// element that continues before it reads this table). [ISELM][ILELM].
constexpr int kMempr[6][6] = {
    {-1, -1, 0, 1, 0, 1},
    {-1, -1, 0, 1, 0, 1},
    {0, 0, -1, -1, -1, -1},
    {0, 0, -1, -1, -1, -1},
    {0, 0, -1, -1, -1, -1},
    {0, 0, -1, -1, -1, -1},
};
// XTRANS: nominal durations in dits, and the density sharpness.
constexpr int   kKimap[6] = {1, 3, 1, 3, 7, 14};
constexpr float kAparm[3] = {3.0f, 1.5f, 1.0f};

constexpr int isx(int k) { return kIsx[k]; }
constexpr int ilami(int l) { return kIlami[l]; }
constexpr int ilamx(int e) { return kIsx[e]; }
constexpr int memfcn(int l, int k) { return kMemfcn[k][l]; }
constexpr float elemtr(int l, int k) { return kElemtr[k][l]; }
constexpr float rtrans(int i, int ind) { return kRtrans[ind][i]; }
constexpr int memdel(int ilelm, int iselm) { return kMemdel[iselm][ilelm]; }
constexpr int mempr(int ilelm, int iselm) { return kMempr[iselm][ilelm]; }

// TRANSL's code tree (morse-wip transl.cxx): node → dit child, dah child,
// text. 0 is the root, and a child 0 means none: a code that goes there
// lands in kSink, which keeps it until the character ends (morse-wip goes
// on from the root; BellConfig::port_tree). Two links differ from
// morse-wip: Z's dah child (Z had none, so ',' could never be read) and
// Q's dit child (pointed at '0').
struct TreeNode {
    int dit, dah;
    const char *chr;
};
constexpr TreeNode kTree[] = {
    {1, 2, "#"},              //  0 root
    {3, 4, "E"},              //  1 .
    {5, 6, "T"},              //  2 -
    {7, 8, "I"},              //  3 ..
    {9, 10, "A"},             //  4 .-
    {11, 12, "N"},            //  5 -.
    {13, 14, "M"},            //  6 --
    {15, 16, "S"},            //  7 ...
    {17, 18, "U"},            //  8 ..-
    {19, 20, "R"},            //  9 .-.
    {21, 22, "W"},            // 10 .--
    {23, 24, "D"},            // 11 -..
    {25, 26, "K"},            // 12 -.-
    {27, 28, "G"},            // 13 --.
    {29, 30, "O"},            // 14 ---
    {31, 32, "H"},            // 15 ....
    {33, 34, "V"},            // 16 ...-
    {35, 36, "F"},            // 17 ..-.
    {37, 38, "*..--*"},       // 18 ..--
    {39, 40, "L"},            // 19 .-..
    {41, 42, "\xC3\x84"},     // 20 .-.-   Ä
    {43, 44, "P"},            // 21 .--.
    {45, 46, "J"},            // 22 .---
    {47, 48, "B"},            // 23 -...
    {49, 50, "X"},            // 24 -..-
    {51, 52, "C"},            // 25 -.-.
    {53, 0, "Y"},             // 26 -.--
    {55, 64, "Z"},            // 27 --..   (morse-wip: no dah child)
    {0, 0, "Q"},              // 28 --.-   (morse-wip: dit child 57, '0')
    {54, 0, "\xC3\x96"},      // 29 ---.   Ö
    {56, 57, "*----*"},       // 30 ----
    {0, 0, "5"},              // 31 .....
    {0, 0, "4"},              // 32 ....-
    {0, 60, "*...-.*"},       // 33 ...-.
    {0, 0, "3"},              // 34 ...--
    {0, 0, "e"},              // 35 ..-..
    {0, 0, "*..-.-*"},        // 36 ..-.-
    {58, 0, "*..--.*"},       // 37 ..--.
    {0, 0, "2"},              // 38 ..---
    {0, 0, "*.-...*"},        // 39 .-...
    {0, 0, "e"},              // 40 .-..-
    {0, 63, "<AR>"},          // 41 .-.-.
    {0, 0, "*"},              // 42 .-.--
    {0, 0, "*.--..*"},        // 43 .--..
    {59, 0, "a"},             // 44 .--.-
    {0, 0, "*.---.*"},        // 45 .---.
    {66, 0, "1"},             // 46 .----
    {0, 0, "6"},              // 47 -....
    {61, 0, "<BT>"},          // 48 -...-
    {0, 0, "/"},              // 49 -..-.
    {0, 0, "*-..--*"},        // 50 -..--
    {0, 0, "*-.-..*"},        // 51 -.-..
    {0, 0, "^H"},             // 52 -.-.-
    {0, 0, "<KN>"},           // 53 -.--.
    {0, 0, "8"},              // 54 ---..
    {0, 0, "7"},              // 55 --...
    {0, 0, "9"},              // 56 ----.
    {0, 0, "0"},              // 57 -----
    {0, 0, "?"},              // 58 ..--..
    {0, 0, "@"},              // 59 .--.-.
    {0, 0, "<SK>"},           // 60 ...-.-
    {0, 62, "*-...-.*"},      // 61 -...-.
    {0, 0, "<BK>"},           // 62 -...-.-
    {0, 0, "."},              // 63 .-.-.-
    {0, 65, "*--..-*"},       // 64 --..-
    {0, 0, ","},              // 65 --..--
    {0, 0, "'"},              // 66 .----.
    {67, 67, "#"},            // 67 sink: no such code
};
constexpr int kSink = 67;
static_assert(std::size(kTree) == kSink + 1, "the sink is the tree's last node");

}  // namespace

BellConfig BellConfig::listing() {
    BellConfig c;
    c.clamp_z = false;
    c.wpm_min = 10;
    return c;
}

BellConfig BellConfig::port() {
    BellConfig c;
    c.paths = 20;
    c.wpm_min = 5;
    c.wpm_max = 80;
    c.port_xtrans_sign = true;
    c.port_tree = true;
    c.clamp_z = true;
    return c;
}

void BellDecoder::set_spread(float b) {
    if (!m_cfg.coherent || m_nq != 1) return;
    const double L = m_cfg.coh_match_ms * 1e-3, dt = m_dt * 1e-3, pi2 = 9.869604401089358;
    m_qval[0] = b;
    m_coha[0] = static_cast<float>(std::exp(-pi2 * b * b * L * dt / 2.0));
}

BellDecoder::BellDecoder(const BellConfig &cfg) : m_cfg(cfg) {
    m_cfg.paths = std::clamp(m_cfg.paths, 7, 256);
    m_cfg.ndelay = std::max(m_cfg.ndelay, 2);
    if (m_cfg.coherent) {
        // The classes are spreads; each gets its AR(1) coefficient over one
        // sample, a = ρ(L)^(dt/L) with ρ(τ) = exp(−π²B²τ²/2), L the match lag.
        m_qval = m_cfg.coh_spreads.empty() ? std::vector<float>{m_cfg.coh_spread} : m_cfg.coh_spreads;
        m_dt = m_cfg.coh_dt_ms;
        const double L = m_cfg.coh_match_ms * 1e-3, dt = m_dt * 1e-3;
        for (float b : m_qval) {
            const double pi2 = 9.869604401089358;
            m_coha.push_back(static_cast<float>(std::exp(-pi2 * b * b * L * dt / 2.0)));
        }
        if (m_cfg.coh_imp_eps > 0.0f) {
            m_ln_gauss = std::log1p(-m_cfg.coh_imp_eps);
            m_ln_imp = std::log(m_cfg.coh_imp_eps);
            m_ln_kappa = std::log(m_cfg.coh_imp_kappa);
        }
        // TRELIS's decode delay stays about 1 s.
        m_cfg.ndelay = std::max(2, static_cast<int>(std::lround(m_cfg.ndelay * kSampleMs / m_dt)));
    } else {
        m_qval = m_cfg.q_bank.empty() ? std::vector<float>{m_cfg.mark_q} : m_cfg.q_bank;
    }
    m_nq = static_cast<int>(m_qval.size());
    m_nodes = kNodes * m_nq;
    reset();
}

void BellDecoder::reset() {
    restart();

    // NOISE's DATA.
    std::fill(std::begin(m_ylong), std::end(m_ylong), 1.0f);
    std::fill(std::begin(m_yshort), std::end(m_yshort), 1.0f);
    m_kl = m_ks = 0;
    m_kkl = m_kks = 1;
    m_ymin1 = m_ymin2 = 1.0f;
    m_ymavg = 0.05f;
    m_rn = m_cfg.coherent ? 1.0f : 0.1f;
    m_omega = m_cfg.coh_omega0;
    if (m_cfg.coherent) restart();          // the filters start at P = Ω
}

void BellDecoder::restart() {
    const int np = m_cfg.paths, nn = m_nodes * np;
    // PROCES's DATA: ISAVE/25/, LAMBDA/25*5/, ILRATE/5*10,5*20,…,5*50/,
    // P/750*1./, LAMSAV/750*5/, DUR/25*1000./, ILRSAV/750*20/, PATHSV/25*5/.
    // The listing's letter state 5 (a dah, then an element space) and path 5
    // are 4 here.
    m_isave = np;
    m_lambda.assign(np, 4);
    m_ilrate.resize(np);
    // The listing's 10 … 50 WPM, inside [wpm_min, wpm_max]: a path started
    // outside it could never move (SPDTR gives a step past a limit 0).
    for (int i = 0; i < np; i++)
        m_ilrate[i] = std::clamp((i / 5 % 5 + 1) * 10, m_cfg.wpm_min, m_cfg.wpm_max);
    m_dur.assign(np, 1000.0f);
    // The q classes take turns over the paths (one class: all 0).
    m_qcls.resize(np);
    for (int i = 0; i < np; i++)
        m_qcls[i] = i % m_nq;
    m_qcsav.assign(nn, 0);
    m_pint.assign(m_nodes, 0.0f);
    m_pathsv.assign(np, 4);
    m_sort.assign(np, 0);
    m_p.assign(nn, 1.0f);
    m_lamsav.assign(nn, 4);
    m_dursav.assign(nn, 0.0f);
    m_ilrsav.assign(nn, std::clamp(20, m_cfg.wpm_min, m_cfg.wpm_max));
    m_lkhd.assign(nn, 0.0f);
    m_pin.assign(nn, 0.0f);
    m_psav_node.assign(nn, 0.0f);
    m_psav_path.assign(np, 0.0f);
    m_saved.assign(nn, 0);
    m_iconv.assign(np, 1);
    m_imax = 0;
    m_xhat = 0;
    m_elmhat = kDit;
    m_px = m_spdhat = m_pelm = 0.0f;
    m_qhat = m_qval[0];

    // KALFIL's DATA: YKKIP/25*.5/, PKKIP/25*.1/. Coherent: g = 0 with the
    // stationary variance Ω, no phase known.
    const float y0 = m_cfg.coherent ? 0.0f : 0.5f, p0 = m_cfg.coherent ? m_omega : 0.1f;
    m_ykkip.assign(np, y0);
    m_pkkip.assign(np, p0);
    m_ykksv.assign(nn, y0);
    m_pkksv.assign(nn, p0);
    m_yikip.assign(m_cfg.coherent ? np : 0, 0.0f);
    m_yiksv.assign(m_cfg.coherent ? nn : 0, 0.0f);

    // TRELIS's DATA: PTHTRL/5000*5/, LMDSAV/5000*5/, N/0/: path 4 and letter
    // state 4 here. (morse-wip zeroes both, and its first NDELAY samples read
    // path 0 of its 1-based arrays, out of bounds.)
    m_pthtrl.assign(m_cfg.ndelay * np, 4);
    m_lmdsav.assign(m_cfg.ndelay * np, 4);
    m_ipnod.assign(np, 0);
    m_ltrsv.assign(m_cfg.ndelay, 0);
    m_n = -1;
    m_ndelst = -1;

    m_ixlast = m_curstate = m_newstate = 0;
}

void BellDecoder::push(float zin, std::string &out) {
    float z;
    noise(zin, m_rn, z);
    if (m_cfg.clamp_z)
        z = std::clamp(z, 0.0f, 1.0f);
    proces(z, m_rn, out);
}

void BellDecoder::push_iq(float re, float im, std::string &out, float w) {
    m_wt = m_cfg.coh_repair_weight ? std::clamp(w, 0.0f, 1.0f) : 0.0f;
    m_yr = re;
    m_yi = im;
    proces(0.0f, 1.0f, out);
    // Ω: E|y|² = Ω + 1 in a mark; follow it slowly, weighted by P(key down).
    const float y2 = re * re + im * im;
    m_omega += 0.005f * m_px * (y2 - 1.0f - m_omega);
    m_omega = std::clamp(m_omega, 0.5f, 1e5f);
}

float BellDecoder::amplitude() const {
    if (m_cfg.coherent) return std::hypot(m_ykkip[m_imax], m_yikip[m_imax]);
    return m_ykkip[m_imax];
}

// NOISE (p. 190, and the OCR text of the page the scan lost): the noise
// power is 0.3 × a slow average of the minimum envelope over the last 1 s
// and 0.25 s; 2.4 × that average + 0.05 is subtracted to give a roughly
// zero-mean key-up process. Bell chose the constants empirically for his
// receiver (thesis §VIII); they assume a signal near unit amplitude.
void BellDecoder::noise(float zin, float &rn, float &z) {
    if (++m_kl == kLong) m_kl = 0;
    if (++m_ks == kShort) m_ks = 0;
    m_kkl = std::min(m_kkl + 1, kLong);
    m_kks = std::min(m_kks + 1, kShort);
    if (m_kks > 2) {
        m_ylong[m_kl] = zin;
        m_yshort[m_ks] = zin;
        m_ymin1 = zin;
        m_ymin2 = zin;
    }
    for (int i = 0; i < m_kkl; i++)
        if (m_ylong[i] <= m_ymin1) m_ymin1 = m_ylong[i];
    for (int i = 0; i < m_kks; i++)
        if (m_yshort[i] <= m_ymin2) m_ymin2 = m_yshort[i];
    const float ymin = std::min(m_ymin1, m_ymin2);
    m_ymavg += (ymin - m_ymavg) * 0.004f;
    rn = std::max(0.3f * m_ymavg, 0.005f);
    z = 1.1f * (zin - 2.4f * m_ymavg - 0.05f);
}

// PROCES (p. 167–168).
void BellDecoder::proces(float z, float rn, std::string &out) {
    // For each saved path: transition probabilities, new nodes, likelihoods.
    for (int i = 0; i < m_isave; i++) {
        trprob(i, m_lambda[i], m_dur[i], m_ilrate[i]);
        path(i, m_lambda[i], m_dur[i], m_ilrate[i]);
        likhd(z, rn, i, m_lambda[i], m_dur[i], m_ilrate[i]);
    }
    // Coherent: LIKHD left log-likelihoods; scale by the largest before
    // taking exp, so a strong signal cannot overflow or underflow them all.
    if (m_cfg.coherent) {
        const int ni = m_nodes * m_isave;
        float lmax = kNoLog;
        for (int j = 0; j < ni; j++) lmax = std::max(lmax, m_lkhd[j]);
        for (int j = 0; j < ni; j++)
            m_lkhd[j] = m_lkhd[j] <= kNoLog ? 0.0f : std::exp(m_lkhd[j] - lmax);
    }
    // Posterior of each new node, of the key state, element and speed.
    // When every node scores zero (the products underflow single precision)
    // the listing divides by zero. The paths start over; NOISE's estimate
    // stays, and the letters still in the trellis are lost.
    if (!probp()) {
        restart();
        return;
    }
    sprob();
    m_xhat = m_px > 0.5f ? 1 : 0;
    savep();
    trelis(out);
}

// TRPROB (p. 169–170).
void BellDecoder::trprob(int ip, int lambda, float dur, int ilrate) {
    if (lambda == kNone) {
        for (int n = 0; n < m_nodes; n++) pin(ip, n) = 0.0f;
        return;
    }
    const int ielem = ilami(lambda);
    const float ptrx = static_cast<float>(xtrans(ielem, dur, ilrate));
    float psum = 0.0f;
    float *pint = m_pint.data();
    // The q class: kept while the element lasts (qstay), moved with
    // probability q_switch when it changes (qmove).
    const int qc = m_qcls[ip];
    for (int c = 0; c < m_nq; c++) {
        const float qstay = c == qc ? 1.0f : 0.0f;
        const float qmove = m_nq == 1 ? 1.0f
                            : c == qc ? 1.0f - m_cfg.q_switch
                                      : m_cfg.q_switch / static_cast<float>(m_nq - 1);
        for (int k = 0; k < kElems; k++)
            for (int i = 0; i < kRates; i++)
                ptrans(k, i, lambda, ilrate, ptrx, psum, pint, c * kNodes + i * kElems + k, qstay,
                       qmove);
    }
    // (No guard in the listing: PSUM is never 0 for a valid λ.)
    for (int n = 0; n < m_nodes; n++) pin(ip, n) = psum != 0.0f ? pint[n] / psum : 0.0f;
}

// XTRANS (p. 170): P(the element lasts another sample | it has lasted D0),
// from a density centred on the nominal length with exponential tails. A
// sample is 5 ms (coherent: coh_dt_ms).
// Single precision, like the listing: whether D0 lands exactly on the
// nominal length decides which branch runs.
double BellDecoder::xtrans(int ielem, float d0, int irate) const {
    const int mscale = kKimap[ielem];
    const float rscale = 1200.0f / static_cast<float>(irate);
    const float b0 = d0 / (static_cast<float>(mscale) * rscale);
    const float b1 = (d0 + m_dt) / (static_cast<float>(mscale) * rscale);
    float alpha;
    if (ielem == kPause) alpha = 14.0f * kAparm[2];
    else if (ielem == kWordSpace) alpha = 7.0f * kAparm[1];
    else alpha = static_cast<float>(mscale) * kAparm[0];
    if (ielem != kPause) alpha *= m_cfg.dur_sharpness;

    if (b1 <= 1.0f) {
        const float p1 = 1.0f - 0.5f * std::exp(alpha * (b1 - 1.0f));
        const float p0 = 1.0f - 0.5f * std::exp(alpha * (b0 - 1.0f));
        return p1 / p0;
    }
    if (b0 < 1.0f && b1 > 1.0f) {
        // Listing line 23600: P1=0.5*EXP(-ALPHA*(B1-1.)). morse-wip has
        // -0.5 here (xtrans.cxx:78).
        const float half = m_cfg.port_xtrans_sign ? -0.5f : 0.5f;
        const float p1 = half * std::exp(-alpha * (b1 - 1.0f));
        const float p0 = 1.0f - 0.5f * std::exp(alpha * (b0 - 1.0f));
        return p1 / p0;
    }
    return std::exp(-alpha * (b1 - b0));
}

// PTRANS (p. 171).
void BellDecoder::ptrans(int kelem, int irate, int lambda, int ilrate, float ptrx, float &psum,
                         float *pin, int n, float qstay, float qmove) const {
    const int ilelm = ilami(lambda);
    if (kelem == ilelm) {
        // The same element continues: the key-state probability, no speed
        // or q class change.
        pin[n] = irate == kRateHold ? ptrx * qstay : 0.0f;
    } else {
        const float pelem = elemtr(lambda, kelem);
        const float prate = static_cast<float>(spdtr(irate, ilrate, kelem, ilelm));
        pin[n] = (1.0f - ptrx) * pelem * prate * qmove;
    }
    psum += pin[n];
}

// SPDTR (p. 172).
double BellDecoder::spdtr(int isrt, int ilrt, int iselm, int ilelm) const {
    if (ilelm == iselm) return isrt == kRateHold ? 1.0 : 0.0;
    const int idel = memdel(ilelm, iselm);
    const int ind1 = mempr(ilelm, iselm);
    if (ind1 == kNone) return 0.0;
    const int israte = ilrt + (isrt - kRateHold) * idel;
    // Listing: 10 and 60 WPM; morse-wip: 5 and 80.
    if (israte > m_cfg.wpm_max || israte < m_cfg.wpm_min) return 0.0;
    return rtrans(isrt, ind1);
}

// PATH (p. 173–174).
void BellDecoder::path(int ip, int lambda, float dur, int ilrate) {
    for (int c = 0; c < m_nq; c++)
    for (int i = 0; i < kRates; i++) {
        for (int k = 0; k < kElems; k++) {
            const int n = c * kNodes + i * kElems + k;
            const int j = ip * m_nodes + n;
            m_qcsav[j] = c;
            m_dursav[j] = 0.0f;
            m_ilrsav[j] = ilrate;
            if (lambda == kNone) {
                m_lamsav[j] = kNone;
                continue;
            }
            m_lamsav[j] = memfcn(lambda, k);
            if (m_lamsav[j] == kNone) continue;
            const int ilelm = ilami(lambda);
            const int ixl = ilamx(ilelm);
            const int ixs = isx(k);
            // The duration grows while the key state stays, restarts on a change.
            m_dursav[j] = dur * static_cast<float>(1 - ixs - ixl + 2 * ixs * ixl) + m_dt;
            m_ilrsav[j] = ilrate + (i - kRateHold) * memdel(ilelm, k);
        }
    }
}

// LIKHD (p. 174–175).
void BellDecoder::likhd(float z, float rn, int ip, int lambda, float dur, int ilrate) {
    if (lambda == kNone) {
        // (The listing indexes ILAMI(IELMST(0)) here; the node's PIN is 0.)
        const float none = m_cfg.coherent ? kNoLog : 0.0f;
        for (int n = 0; n < m_nodes; n++) m_lkhd[ip * m_nodes + n] = none;
        return;
    }
    if (m_cfg.coherent) {
        for (int c = 0; c < m_nq; c++)
            for (int k = 0; k < kElems; k++)
                for (int i = 0; i < kRates; i++) {
                    const int n = c * kNodes + i * kElems + k;
                    const int j = ip * m_nodes + n;
                    m_lkhd[j] = kalfil_coh(ip, isx(k), j, pin(ip, n), c);
                }
        return;
    }
    const int kelem = ilami(lambda);
    for (int c = 0; c < m_nq; c++) {
        for (int k = 0; k < kElems; k++) {
            for (int i = 0; i < kRates; i++) {
                const int n = c * kNodes + i * kElems + k;
                const int j = ip * m_nodes + n;
                m_lkhd[j] = kalfil(z, ip, rn, isx(k), kelem, j, dur, ilrate, pin(ip, n), m_qval[c]);
            }
        }
    }
}

// KALFIL (p. 176–177): one scalar Kalman step on the amplitude for this
// extension, and the Gaussian likelihood of the innovation.
float BellDecoder::kalfil(float z, int ip, float rn, int ixs, int kelem, int jnode, float dur,
                          int ilrate, float pin, float qmark) {
    constexpr float kPinMin = 0.0001f;
    if (pin <= kPinMin) {
        // Not scored: the node gets no mass, and SAVEP keeps no node without
        // mass. The filter state is copied so that none is left unset.
        m_ykksv[jnode] = m_ykkip[ip];
        m_pkksv[jnode] = m_pkkip[ip];
        return 0.0f;
    }
    float phi, qa;
    model(dur, kelem, ilrate, ixs, qmark, phi, qa);
    const float hz = static_cast<float>(ixs);
    const float ykk = m_ykkip[ip], pkk = m_pkkip[ip];
    const float ypred = phi * ykk;
    const float ppred = phi * pkk * phi + qa;
    const float pz = hz * ppred + rn;
    const float pzinv = 1.0f / pz;
    const float g = ppred * hz * pzinv;
    const float pest = (1.0f - g * hz) * ppred;
    const float zr = z - hz * ypred;
    m_ykksv[jnode] = std::max(ypred + g * zr, 0.01f);
    m_pkksv[jnode] = pest;
    const float a = 0.5f * pzinv * zr * zr;
    if (a > 1000.0f) return 0.0f;
    return std::exp(-a) / std::sqrt(pz);
}

// The coherent KALFIL: g' = a·g + v, v ~ CN(0, (1 − a²)·Ω); in a mark
// y = g + w with w ~ CN(0, 1), in a space y = w and the filter only predicts.
// Returns ln p(y) less ln π, which every node shares.
float BellDecoder::kalfil_coh(int ip, int ixs, int jnode, float pin, int c) {
    constexpr float kPinMin = 0.0001f;
    if (pin <= kPinMin) {
        m_ykksv[jnode] = m_ykkip[ip];
        m_yiksv[jnode] = m_yikip[ip];
        m_pkksv[jnode] = m_pkkip[ip];
        return kNoLog;
    }
    const float a = m_coha[c];
    const float gr = m_known ? m_kgr : a * m_ykkip[ip], gi = m_known ? m_kgi : a * m_yikip[ip];
    const float ppred = m_known ? 0.0f : a * a * m_pkkip[ip] + (1.0f - a * a) * m_omega;
    const bool imp = m_cfg.coh_imp_eps > 0.0f;
    const float kappa = m_cfg.coh_imp_kappa;
    // ln(e^a + e^b), for the impulse mixture.
    auto lse = [](float a, float b) { return a > b ? a + std::log1p(std::exp(b - a)) : b + std::log1p(std::exp(a - b)); };
    // A sample the impulse repair rebuilt (weight m_wt) carries 1 − m_wt of
    // its evidence: its log-likelihood and its Kalman gain are scaled by it.
    const float tw = 1.0f - m_wt;
    // The noise both hypotheses score with: 1, or the path's predicted mark
    // power over coh_floor_ratio where that is more. Without it a strong
    // station's samples mismatch either hypothesis by far more than the
    // unit noise allows at every keying edge (as v3's tracker,
    // SKIM_V3_TRACK_RATIO).
    const float sn = m_cfg.coh_floor_ratio > 0.0f
                         ? std::max(1.0f, (gr * gr + gi * gi + ppred) / m_cfg.coh_floor_ratio) : 1.0f;
    if (ixs == 0) {
        m_ykksv[jnode] = gr;
        m_yiksv[jnode] = gi;
        m_pkksv[jnode] = ppred;
        const float y2 = m_yr * m_yr + m_yi * m_yi;
        if (sn > 1.0f && !imp) {
            const float l = -y2 / sn - std::log(sn);
            return m_wt > 0.0f ? tw * l : l;
        }
        if (!imp) return m_wt > 0.0f ? -tw * y2 : -y2;
        const float l = lse(m_ln_gauss - y2, m_ln_imp - y2 / kappa - m_ln_kappa);
        return m_wt > 0.0f ? tw * l : l;
    }
    const float s = ppred + sn, er = m_yr - gr, ei = m_yi - gi, e2 = er * er + ei * ei;
    const float k = m_wt > 0.0f ? tw * (ppred / s) : ppred / s;
    if (!imp) {
        m_ykksv[jnode] = gr + k * er;
        m_yiksv[jnode] = gi + k * ei;
        m_pkksv[jnode] = (1.0f - k) * ppred;
        const float l = -e2 / s - std::log(s);
        return m_wt > 0.0f ? tw * l : l;
    }
    // Two updates, one per noise component, mixed by the posterior weight r
    // of the impulse component.
    const float si = ppred + kappa, ki = ppred / si;
    const float lg = m_ln_gauss - e2 / s - std::log(s);
    const float li = m_ln_imp - e2 / si - std::log(si);
    const float l = lse(lg, li);
    const float r = m_cfg.coh_imp_update ? std::exp(li - l) : 0.0f;
    const float keff = (1.0f - r) * k + r * (m_wt > 0.0f ? tw * ki : ki);
    m_ykksv[jnode] = gr + keff * er;
    m_yiksv[jnode] = gi + keff * ei;
    m_pkksv[jnode] = m_wt > 0.0f ? (1.0f - keff) * ppred : ((1.0f - r) * (1.0f - k) + r * (1.0f - ki)) * ppred;
    return m_wt > 0.0f ? tw * l : l;
}

// MODEL (p. 177–178): during a mark the amplitude is a slow random walk;
// during a space it decays towards 0 with no process noise; entering a mark
// after a space adds variance that grows with the space's length.
void BellDecoder::model(float dur, int ielm, int ilr, int ixs, float qmark, float &phi,
                        float &qa) const {
    const float r1 = 1200.0f / static_cast<float>(ilr);
    const float bauds = std::min(dur / r1, 14.0f);
    if (ielm <= kDah) {
        qa = qmark;
        phi = 1.0f;
        return;
    }
    if (ixs != 0) {
        phi = 1.0f;
        qa = 0.15f * std::exp(0.6f * (bauds - 14.0f));
        qa += 0.01f * bauds * std::exp(0.2f * (1.0f - bauds));
        return;
    }
    const float xsamp = 22.4f * r1;
    phi = bauds >= 14.0f ? 1.0f : std::pow(10.0f, -2.0f / xsamp);
    qa = 0.0f;
}

// PROBP (p. 178–179). false when no node has any mass.
bool BellDecoder::probp() {
    const int ni = m_nodes * m_isave;
    std::vector<float> &psav = m_psav_node;
    double psum = 0.0;
    for (int i = 0; i < m_isave; i++) {
        for (int n = 0; n < m_nodes; n++) {
            const int j = i * m_nodes + n;
            psav[j] = m_p[i] * pin(i, n) * m_lkhd[j];
            psum += psav[j];
        }
    }
    if (!(psum > 0.0)) return false;
    for (int j = 0; j < ni; j++) m_p[j] = static_cast<float>(psav[j] / psum);
    return true;
}

// SPROB (p. 179–180).
void BellDecoder::sprob() {
    float pselem[kElems] = {};
    m_spdhat = 0.0f;
    m_px = 0.0f;
    float qhat = 0.0f;
    for (int k = 0; k < kElems; k++) {
        for (int c = 0; c < m_nq; c++) {
            for (int i = 0; i < kRates; i++) {
                const int n = c * kNodes + i * kElems + k;
                for (int m = 0; m < m_isave; m++) {
                    const int j = m * m_nodes + n;
                    pselem[k] += m_p[j];
                    m_spdhat += static_cast<float>(m_ilrsav[j]) * m_p[j];
                    if (k <= kDah) m_px += m_p[j];
                    qhat += m_qval[c] * m_p[j];
                }
            }
        }
    }
    m_qhat = m_nq == 1 ? m_qval[0] : qhat;
    m_pelm = 0.0f;
    for (int k = 0; k < kElems; k++) {
        if (pselem[k] >= m_pelm) {
            m_pelm = pselem[k];
            m_elmhat = k;
        }
    }
}

// SAVEP (p. 180–183): keep the best node of each element type, then the
// best of the rest until POPT of the mass or PATHS nodes are kept; merge
// nodes with the same (λ, duration, speed, q class), keeping the more
// probable.
void BellDecoder::savep() {
    const int np = m_cfg.paths;
    std::vector<float> &psav = m_psav_path;
    // SORT(1..NSAV) as a flag per node: the listing searches SORT for every
    // node it looks at.
    std::fill(m_saved.begin(), m_saved.begin() + m_nodes * m_isave, 0);
    int nsav = 0;
    double psum = 0.0;
    int jsav = 0, ipsav = 0;

    for (int k = 0; k < kElems; k++) {
        float pmax = 0.0f;
        for (int ip = 0; ip < m_isave; ip++) {
            for (int c = 0; c < m_nq; c++) {
                for (int i = 0; i < kRates; i++) {
                    const int j = ip * m_nodes + c * kNodes + i * kElems + k;
                    if (m_p[j] >= pmax) {
                        pmax = m_p[j];
                        jsav = j;
                        ipsav = ip;
                    }
                }
            }
        }
        if (pmax >= 0.000001f) {
            psum += pmax;
            psav[nsav] = pmax;
            m_pathsv[nsav] = ipsav;
            m_sort[nsav] = jsav;
            m_saved[jsav] = 1;
            nsav++;
        }
    }

    do {
        float pmax = 0.0f;
        bool found = false;
        for (int ip = 0; ip < m_isave; ip++) {
            for (int n = 0; n < m_nodes; n++) {
                const int j = ip * m_nodes + n;
                if (m_saved[j]) continue;
                if (m_p[j] > pmax) {
                    pmax = m_p[j];
                    jsav = j;
                    ipsav = ip;
                    found = true;
                }
            }
        }
        // (The listing saves the previous JSAV again when nothing is left;
        // the merge below then drops the copy. Stop instead.)
        if (!found) break;
        psum += pmax;
        psav[nsav] = pmax;
        m_pathsv[nsav] = ipsav;
        m_sort[nsav] = jsav;
        m_saved[jsav] = 1;
        nsav++;
    } while (psum < m_cfg.popt && nsav < np);

    m_isave = nsav;
    for (int i = 0; i < m_isave; i++) {
        m_p[i] = static_cast<float>(psav[i] / psum);
        m_lambda[i] = m_lamsav[m_sort[i]];
        m_dur[i] = m_dursav[m_sort[i]];
        m_ilrate[i] = m_ilrsav[m_sort[i]];
        m_qcls[i] = m_qcsav[m_sort[i]];
        m_ykkip[i] = m_ykksv[m_sort[i]];
        if (m_cfg.coherent) m_yikip[i] = m_yiksv[m_sort[i]];
        m_pkkip[i] = m_pkksv[m_sort[i]];
    }

    // Merge identical nodes: the first of each kind stays, the others go,
    // and their mass with them.
    std::vector<char> &iconv = m_iconv;
    std::fill(iconv.begin(), iconv.begin() + m_isave, 1);
    for (int n = 0; n < m_isave - 1; n++) {
        if (!iconv[n]) continue;
        for (int k = n + 1; k < m_isave; k++) {
            if (iconv[k] && m_ilrate[k] == m_ilrate[n] && m_dur[k] == m_dur[n] &&
                m_lambda[k] == m_lambda[n] && m_qcls[k] == m_qcls[n])
                iconv[k] = 0;
        }
    }
    // The listing starts this sum at 0, leaving P(1), the first node, out of
    // the normalisation (line 40800); morse-wip starts it at P(1), as here.
    psum = m_p[0];
    int n = 1;
    for (int i = 1; i < m_isave; i++) {
        if (!iconv[i]) continue;
        m_lambda[n] = m_lambda[i];
        m_dur[n] = m_dur[i];
        m_ilrate[n] = m_ilrate[i];
        m_qcls[n] = m_qcls[i];
        m_ykkip[n] = m_ykkip[i];
        if (m_cfg.coherent) m_yikip[n] = m_yikip[i];
        m_pkkip[n] = m_pkkip[i];
        m_pathsv[n] = m_pathsv[i];
        m_sort[n] = m_sort[i];
        m_p[n] = m_p[i];
        psum += m_p[n];
        n++;
    }
    m_isave = n;
    float pmax = 0.0f;
    m_imax = 0;
    for (int i = 0; i < m_isave; i++) {
        m_p[i] = static_cast<float>(m_p[i] / psum);
        if (m_p[i] > pmax) {
            pmax = m_p[i];
            m_imax = i;
        }
    }
}

// TRELIS (p. 184–186): the saved nodes go into a circular trellis of
// NDELAY stages; when the paths back from every saved node meet, the letter
// states from the previous meeting point to this one are final. Without a
// meeting within NDELAY, the best path is read and the others are pruned.
void BellDecoder::trelis(std::string &out) {
    const int ndelay = m_cfg.ndelay;
    // The stage written d samples ago.
    auto stage = [this, ndelay](int d) { return m_n - d < 0 ? m_n - d + ndelay : m_n - d; };

    if (++m_n == ndelay) m_n = 0;
    for (int i = 0; i < m_isave; i++) {
        pthtrl(m_n, i) = m_pathsv[i];
        lmdsav(m_n, i) = m_lambda[i];
    }

    // m_ipnod: where each saved path stood d stages back.
    for (int i = 0; i < m_isave; i++) m_ipnod[i] = i;
    int ipmax = m_imax, ndel = 0;
    for (int d = 0;; d++) {
        if (d == ndelay - 1) {
            // No convergence within NDELAY: read the best path.
            ndel = d;
            transl(lmdsav(stage(d), ipmax), out);
            for (int j = 0; j < m_isave; j++)
                if (m_ipnod[j] != ipmax) m_lambda[j] = kNone;
            break;
        }
        const int s = stage(d);
        for (int ip = 0; ip < m_isave; ip++) {
            m_ipnod[ip] = pthtrl(s, m_ipnod[ip]);
            if (ip == m_imax) ipmax = m_ipnod[ip];
        }
        const int ip0 = m_ipnod[0];
        if (!std::all_of(m_ipnod.begin() + 1, m_ipnod.begin() + m_isave,
                         [ip0](int ip) { return ip == ip0; }))
            continue;

        // The paths meet d + 1 stages back. The previous meeting point is
        // now m_ndelst + 1 stages back, so the stages from here to m_ndelst
        // are final; none when the meeting point kept its place. Read them
        // back along the path, then translate them in time order.
        ndel = d + 1;
        int kd = 0, ipath = ip0;
        for (int dd = ndel; dd <= m_ndelst; dd++) {
            const int ii = stage(dd);
            m_ltrsv[kd++] = lmdsav(ii, ipath);
            ipath = pthtrl(ii, ipath);
        }
        while (kd > 0) transl(m_ltrsv[--kd], out);
        break;
    }
    m_ndelst = ndel;
}

// TRANSL: morse-wip's. The listing's TRANSL (p. 186–188) maps letter states
// of the tree model through LTRMAP; the 16-state model's states are element
// pairs, so a code tree assembles them: a mark start steps the tree, a
// character space, word space or pause after a mark emits the character.
// (morse-wip writes each character over the previous one in one buffer, so
// a trellis step that settles several letters keeps only the last; here
// they are appended.)
void BellDecoder::transl(int ltr, std::string &out) {
    if (ltr < 0 || ltr >= kStates) return;
    const int elm = ilami(ltr);
    const int ixl = ilamx(elm);
    const auto step = [this](int child) { return child != 0 || m_cfg.port_tree ? child : kSink; };
    if (ixl != m_ixlast) {
        switch (elm) {
        case kDit:
            m_newstate = step(kTree[m_curstate].dit);
            break;
        case kDah:
            m_newstate = step(kTree[m_curstate].dah);
            break;
        case kCharSpace:
            out += kTree[m_curstate].chr;
            m_newstate = 0;
            break;
        case kWordSpace:
        case kPause:
            out += kTree[m_curstate].chr;
            out += ' ';
            m_newstate = 0;
            break;
        default:                                       // element space
            break;
        }
    }
    m_curstate = m_newstate;
    m_ixlast = ixl;
}
