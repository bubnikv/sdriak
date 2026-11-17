/*
 * cw_morse.cpp — see cw_morse.h. The table is upstream's (decode_cw_v2.c,
 * "v1's, duplicated so v1 stays untouched").
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "cw_morse.h"

namespace {

struct MorseEntry { char c; const char *m; };

const MorseEntry MORSE[] = {
    {'A',".-"},   {'B',"-..."}, {'C',"-.-."}, {'D',"-.."},  {'E',"."},
    {'F',"..-."}, {'G',"--."},  {'H',"...."}, {'I',".."},   {'J',".---"},
    {'K',"-.-"},  {'L',".-.."}, {'M',"--"},   {'N',"-."},   {'O',"---"},
    {'P',".--."}, {'Q',"--.-"}, {'R',".-."},  {'S',"..."},  {'T',"-"},
    {'U',"..-"},  {'V',"...-"}, {'W',".--"},  {'X',"-..-"}, {'Y',"-.--"},
    {'Z',"--.."},
    {'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},
    {'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
    {'/',"-..-."},{'?',"..--.."},{'=',"-...-"},{'+',".-.-."},{'.',".-.-.-"},
    {',',"--..--"},{'-',"-....-"},{'@',".--.-."},
};

// code → char and char → pattern, built once (thread-safe static init).
struct Tables {
    char lut[256] = {};
    const char *str[128] = {};
    Tables() {
        for (const MorseEntry &e : MORSE) {
            str[static_cast<unsigned char>(e.c)] = e.m;
            unsigned code = 1;
            for (const char *p = e.m; *p; p++) {
                code = (code << 1) | (*p == '-' ? 1u : 0u);
            }
            lut[code] = e.c;
        }
    }
};

const Tables &tables() {
    static const Tables t;
    return t;
}

}  // namespace

char CwMorse::decode(unsigned code) {
    return code < std::size(tables().lut) ? tables().lut[code] : 0;
}

const char *CwMorse::pattern(unsigned char c) {
    return c < 128 ? tables().str[c] : nullptr;
}

char CwMorse::nearest(unsigned code, unsigned nelem) {
    if (nelem < 2 || nelem > 7)
        return 0;
    for (int tier = 0; tier < 2; tier++) {
        char found = 0;
        bool many = false;
        for (unsigned i = 0; i < nelem && !many; i++) {
            unsigned c2;
            if (tier == 0) {                         // drop element i (from MSB)
                const unsigned lo = code & ((1u << (nelem - 1 - i)) - 1);
                const unsigned hi = code >> (nelem - i);
                c2 = (hi << (nelem - 1 - i)) | lo;
            } else {                                 // flip element i
                c2 = code ^ (1u << (nelem - 1 - i));
            }
            const char ch = decode(c2);
            if (ch) {
                if (found && found != ch) { many = true; }
                found = ch;
            }
        }
        if (found && !many)
            return found;
        if (many)
            return 0;
    }
    return 0;
}
