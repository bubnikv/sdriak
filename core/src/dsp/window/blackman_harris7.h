/*
 * Adapted from the SDRPP distribution (https://github.com/qrp73/SDRPP).
 * Copyright (c) 2023 qrp73.
 * Modified for SDRIAK, 2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once
#include "cosine.h"

namespace dsp::window {
    // 7-term Blackman-Harris (P. Albrecht), ~-180 dB sidelobes, NENBW 2.63 bins
    inline double blackmanHarris7(double n, double N) {
        const double coefs[] = {
            0.27105140069342,
            0.43329793923448,
            0.21812299954311,
            0.06592544638803,
            0.01081174209837,
            0.00077658482522,
            0.00001388721735
        };
        return cosine(n, N, coefs, sizeof(coefs) / sizeof(double));
    }
}
