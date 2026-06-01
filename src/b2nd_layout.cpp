/*********************************************************************
 * blosc2_htj2k: inline B2ND metalayer reader.
 *
 * Use c-blosc2's header-only b2nd_deserialize_meta_inline() so the codec
 * does not need the non-inline b2nd_deserialize_meta() runtime symbol.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "b2nd_layout.h"

#include <cstdlib>
#include <limits>

namespace blosc2_htj2k_detail {
namespace {

bool read_schunk_block_layout(blosc2_cparams *cparams, B2ndLayout &layout) {
    if (cparams == nullptr || cparams->schunk == nullptr) {
        return false;
    }
    const auto *schunk = static_cast<const blosc2_schunk *>(cparams->schunk);
    if (schunk->typesize <= 0 ||
        schunk->ndim <= 0 ||
        schunk->ndim > BLOSC2_HTJ2K_LAYOUT_MAX_DIM ||
        schunk->blockshape == nullptr) {
        return false;
    }

    B2ndLayout candidate;
    candidate.ndim = schunk->ndim;
    candidate.typesize = schunk->typesize;
    // HDF5/libh5blosc2 can call codecs with raw chunks and no B2ND
    // metalayer. In that path it still stores the HDF5 chunk geometry in
    // schunk->blockshape. The logical array shape is not needed by this
    // codec, so use the chunk/block geometry for all layout fields.
    for (int i = 0; i < candidate.ndim; ++i) {
        int64_t v = schunk->blockshape[i];
        if (v <= 0 || v > (std::numeric_limits<int32_t>::max)()) {
            return false;
        }
        candidate.shape[i] = v;
        candidate.chunkshape[i] = static_cast<int32_t>(v);
        candidate.blockshape[i] = static_cast<int32_t>(v);
    }
    layout = candidate;
    return true;
}

bool parse_b2nd_meta_inline(const uint8_t *content, int32_t content_len, B2ndLayout &layout) {
    int8_t ndim = 0;
    int64_t shape[B2ND_MAX_DIM] = {};
    int32_t chunkshape[B2ND_MAX_DIM] = {};
    int32_t blockshape[B2ND_MAX_DIM] = {};
    char *dtype = nullptr;
    int8_t dtype_format = 0;

    int rc = b2nd_deserialize_meta_inline(content, content_len, &ndim, shape,
                                          chunkshape, blockshape, &dtype, &dtype_format);
    if (rc < 0 || ndim <= 0 || ndim > BLOSC2_HTJ2K_LAYOUT_MAX_DIM) {
        free(dtype);
        return false;
    }

    B2ndLayout candidate;
    candidate.ndim = ndim;
    candidate.dtype_format = dtype_format;
    if (dtype != nullptr) {
        candidate.dtype = dtype;
    }
    free(dtype);

    for (int i = 0; i < ndim; ++i) {
        candidate.shape[i] = shape[i];
        candidate.chunkshape[i] = chunkshape[i];
        candidate.blockshape[i] = blockshape[i];
    }
    layout = candidate;
    return true;
}

}  // namespace

bool read_b2nd_layout(blosc2_cparams *cparams, B2ndLayout &layout) {
    if (cparams == nullptr || cparams->schunk == nullptr) {
        return false;
    }

    uint8_t *content = nullptr;
    int32_t content_len = 0;
    if (blosc2_meta_get((blosc2_schunk *)cparams->schunk, "b2nd",
                        &content, &content_len) < 0) {
        return read_schunk_block_layout(cparams, layout);
    }

    bool ok = parse_b2nd_meta_inline(content, content_len, layout);
    free(content);
    if (!ok) {
        return read_schunk_block_layout(cparams, layout);
    }
    layout.typesize = static_cast<int32_t>(((blosc2_schunk *)cparams->schunk)->typesize);
    return layout.typesize > 0;
}

bool image_layout_from_b2nd(const B2ndLayout &layout,
                            int64_t &dim_x,
                            int64_t &dim_y,
                            int32_t &num_comps) {
    uint32_t igdim = 0;
    for (int i = 0; i < layout.ndim; ++i) {
        if (layout.blockshape[i] == 1) {
            igdim++;
        } else {
            break;
        }
    }
    if ((layout.ndim - static_cast<int8_t>(igdim)) < 2) {
        return false;
    }

    dim_y = layout.blockshape[igdim];
    dim_x = layout.blockshape[igdim + 1];
    num_comps = 1;
    if ((layout.ndim - static_cast<int8_t>(igdim)) == 3) {
        num_comps = layout.blockshape[igdim + 2];
    }
    return dim_x > 0 && dim_y > 0 && num_comps > 0;
}

}  // namespace blosc2_htj2k_detail
