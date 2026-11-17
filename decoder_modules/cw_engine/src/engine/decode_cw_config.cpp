/*
 * decode_cw_config.cpp — see decode_cw_config.h.
 *
 * Each switch is parsed exactly as upstream's decode_cw_v2.c / _v3.c parse
 * it at first use (the comment names the upstream reader); a variable that
 * is not set leaves the base value.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "decode_cw_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// The variable's value, nullptr when unset.
const char *env(const std::string &name) { return std::getenv(name.c_str()); }

bool set_nonempty(const char *e) { return e && e[0]; }

// "on unless 0": set, non-empty and not "0" (edge_on, NEAREST, RAYCAP).
bool flag(const char *e) { return set_nonempty(e) && strcmp(e, "0") != 0; }

// upstream v3's env_num(): unparsable text keeps the default.
double num_strict(const char *e, double dflt) {
    if (!set_nonempty(e))
        return dflt;
    char *end = nullptr;
    const double v = std::strtod(e, &end);
    return end && end != e ? v : dflt;
}

}  // namespace

CwDecoderConfig cw_preset_v2() {
    CwDecoderConfig c;
    c.v3.robust_clock = false;
    c.v3.front.on = false;
    c.v3.reader.on = false;
    return c;
}

CwDecoderConfig cw_preset_v3() { return CwDecoderConfig(); }

CwDecoderConfig cw_config_from_env(CwDecoderConfig c, int version) {
    const std::string v = version == 3 ? "SKIM_V3_" : "SKIM_V2_";
    const std::string cw = version == 3 ? "SKIM_CW3_" : "SKIM_CW2_";
    const char *e;

    // --- diagnostics: cw2_dbg(), soft_step's trace, kws_eval() (the run dump:
    // CwRunDumpSink::from_env(), the oracle: SkimOracle::from_env())
    if ((e = env(cw + "DEBUG"))) {
        c.diag.debug = true;
        c.diag.debug_hz = std::strtod(e, nullptr);
    }
    if (env(cw + "TRACE")) { c.diag.trace = true; }
    if (env("SKIM_ORACLE_KWS_DUMP")) { c.diag.kws_dump = true; }

    // --- shared -----------------------------------------------------------
    if (set_nonempty(e = env(v + "NARROW"))) { c.envelope.narrow_fc = std::strtod(e, nullptr); }
    if ((e = env(v + "NEAREST"))) { c.assembly.nearest = flag(e); }
    if ((e = env(v + "EDGE"))) { c.lattice.edge = flag(e); }
    if (set_nonempty(e = env(v + "EDGE_WEAK"))) { c.lattice.edge_weak_db = std::strtod(e, nullptr); }
    if (set_nonempty(e = env(v + "DAHMAX"))) { c.lattice.dah_max = std::strtod(e, nullptr); }
    if ((e = env(v + "RESCORE"))) {               // rs_cfg()
        c.rescore.on = !e[0] || strcmp(e, "0") != 0;
        if (c.rescore.on && strchr(e, ',')) {
            sscanf(e, "%lf,%lf,%lf", &c.rescore.max_deficit, &c.rescore.min_margin, &c.rescore.min_evidence);
        }
    }
    if (env(v + "RESCORE_DEBUG")) { c.rescore.debug = true; }
    if (set_nonempty(e = env(v + "RELOCK_DB"))) { c.clock.relock_db = std::strtod(e, nullptr); }
    if ((e = env(v + "RAYFADE"))) {               // soft_step: "r0,r1[,wmin]"
        c.llr.rayfade = e[0] &&
                    sscanf(e, "%lf,%lf,%lf", &c.llr.rayfade_r0, &c.llr.rayfade_r1, &c.llr.rayfade_wmin) >= 2 &&
                    c.llr.rayfade_r1 > c.llr.rayfade_r0;
    }
    if (set_nonempty(e = env(v + "RAYSMEAR"))) {  // soft_step: "s0,s1"
        c.llr.raysmear = sscanf(e, "%lf,%lf", &c.llr.raysmear_s0, &c.llr.raysmear_s1) == 2 &&
                     c.llr.raysmear_s1 > c.llr.raysmear_s0;
    }
    if ((e = env(v + "RAYCAP"))) { c.llr.raycap = flag(e); }

    // --- v3 only ----------------------------------------------------------
    if (version == 3) {
        if ((e = env("SKIM_V3_FRONT"))) {           // fe_front_on(): "v2" = off
            c.v3.front.on = !(std::tolower(static_cast<unsigned char>(e[0])) == 'v' && e[1] == '2' && !e[2]);
        }
        c.v3.front.det_db      = num_strict(env("SKIM_V3_DET_DB"), c.v3.front.det_db);
        c.v3.weak.dead_s      = num_strict(env("SKIM_V3_DEAD_S"), c.v3.weak.dead_s);
        c.v3.front.det_s       = num_strict(env("SKIM_V3_DET_S"), c.v3.front.det_s);
        c.v3.weak.preroll_x   = num_strict(env("SKIM_V3_PREROLL_X"), c.v3.weak.preroll_x);
        c.v3.front.weak_db     = num_strict(env("SKIM_V3_WEAK_DB"), c.v3.front.weak_db);
        c.v3.weak.min_db      = num_strict(env("SKIM_V3_MIN_DB"), c.v3.weak.min_db);
        c.rescore.min_evidence_weak = num_strict(env("SKIM_V3_RS_EV_WEAK"), c.rescore.min_evidence_weak);
        c.v3.weak.fist_hold_s = num_strict(env("SKIM_V3_FIST_HOLD_S"), c.v3.weak.fist_hold_s);
        if ((e = env("SKIM_V3_KWS"))) { c.v3.reader.on = strcmp(e, "0") != 0; }   // kr_cfg()
        // kr_num(): upstream takes strtod() as is here, unparsable reads 0
        if (set_nonempty(e = env("SKIM_V3_KWS_DC"))) { c.v3.reader.min_score = std::strtod(e, nullptr); }
        if (set_nonempty(e = env("SKIM_V3_KWS_SEP"))) { c.v3.reader.min_margin = std::strtod(e, nullptr); }
        if (env("SKIM_V3_KWS_DEBUG")) { c.v3.reader.debug = true; }
    }
    return c;
}
