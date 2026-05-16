/*********************************************************************
 * blosc2_htj2k: local B2ND metalayer reader.
 *
 * Keep the codec independent from non-inline B2ND symbols at runtime.  HDF5
 * can load blosc2_htj2k as a native codec before Python imports this package;
 * in that path the host process may not expose b2nd_deserialize_meta() with
 * global visibility.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#ifndef BLOSC2_HTJ2K_B2ND_LAYOUT_H
#define BLOSC2_HTJ2K_B2ND_LAYOUT_H

#include <array>
#include <cstdint>
#include <string>

#include "blosc2.h"
#include "b2nd.h"

#ifdef BLOSC2_MAX_DIM
#define BLOSC2_HTJ2K_LAYOUT_MAX_DIM BLOSC2_MAX_DIM
#else
#define BLOSC2_HTJ2K_LAYOUT_MAX_DIM B2ND_MAX_DIM
#endif

namespace blosc2_htj2k_detail {

struct B2ndLayout {
    int8_t ndim = 0;
    std::array<int64_t, BLOSC2_HTJ2K_LAYOUT_MAX_DIM> shape{};
    std::array<int32_t, BLOSC2_HTJ2K_LAYOUT_MAX_DIM> chunkshape{};
    std::array<int32_t, BLOSC2_HTJ2K_LAYOUT_MAX_DIM> blockshape{};
    std::string dtype;
    int8_t dtype_format = 0;
    int32_t typesize = 0;
};

bool read_b2nd_layout(blosc2_cparams *cparams, B2ndLayout &layout);

bool image_layout_from_b2nd(const B2ndLayout &layout,
                            int64_t &dim_x,
                            int64_t &dim_y,
                            int32_t &num_comps);

}  // namespace blosc2_htj2k_detail

#endif  // BLOSC2_HTJ2K_B2ND_LAYOUT_H
