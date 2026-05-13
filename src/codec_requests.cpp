/*********************************************************************
 * blosc2_grok: request builders for J2K and HTJ2K replacement backends.
 *
 * This file owns the translation from Blosc2/Grok call context to the small
 * C request structs used by replacement plugins.  It does not load plugins or
 * call codec implementations.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "codec_requests.h"

#include <cstdlib>

#include "b2nd_layout.h"

namespace blosc2_grok_detail {
namespace {

// Extract the image-like layout from b2nd metadata for plugin capability
// checks.  Backends still deserialize the metadata themselves when they need
// full geometry.
bool read_b2nd_codec_layout(blosc2_cparams *cparams,
                            uint32_t &precision_bits,
                            uint32_t &num_components) {
    precision_bits = 0;
    num_components = 0;

    B2ndLayout layout;
    if (!read_b2nd_layout(cparams, layout)) {
        return false;
    }
    int64_t dim_x = 0;
    int64_t dim_y = 0;
    int32_t comps = 0;
    if (!image_layout_from_b2nd(layout, dim_x, dim_y, comps)) {
        return false;
    }
    precision_bits = static_cast<uint32_t>(8 * layout.typesize);
    num_components = static_cast<uint32_t>(comps);
    return true;
}

// Extract common sample layout into a J2K plugin request.
void fill_j2k_layout(j2k_codec_request_t &request, blosc2_cparams *cparams) {
    uint32_t precision_bits = 0;
    uint32_t num_components = 0;
    if (read_b2nd_codec_layout(cparams, precision_bits, num_components)) {
        request.precision_bits = precision_bits;
        request.num_components = num_components;
    } else if (cparams != nullptr && cparams->schunk != nullptr) {
        const auto *schunk = (const blosc2_schunk*)cparams->schunk;
        request.precision_bits = static_cast<uint32_t>(8 * schunk->typesize);
    }
}

// Extract common sample layout into an HTJ2K plugin request.
void fill_htj2k_layout(htj2k_codec_request_t &request, blosc2_cparams *cparams) {
    uint32_t precision_bits = 0;
    uint32_t num_components = 0;
    if (read_b2nd_codec_layout(cparams, precision_bits, num_components)) {
        request.precision_bits = precision_bits;
        request.num_components = num_components;
    } else if (cparams != nullptr && cparams->schunk != nullptr) {
        const auto *schunk = (const blosc2_schunk*)cparams->schunk;
        request.precision_bits = static_cast<uint32_t>(8 * schunk->typesize);
    }
}

}  // namespace

bool is_htj2k_requested(const grk_cparameters *params) {
    if (!params) {
        return false;
    }
    return ((params->cblk_sty & (GRK_CBLKSTY_HT | GRK_CBLKSTY_HT_MIXED | GRK_CBLKSTY_HT_PHLD)) != 0) ||
           ((params->rsiz & GRK_JPH_RSIZ_FLAG) != 0);
}

j2k_codec_request_t make_j2k_encode_request(uint8_t meta,
                                            blosc2_cparams *cparams,
                                            const void *chunk,
                                            const grk_cparameters *compress_params) {
    j2k_codec_request_t request = {};
    request.struct_size = sizeof(j2k_codec_request_t);
    request.meta = meta;
    request.cparams = cparams;
    request.chunk = chunk;
    fill_j2k_layout(request, cparams);

    if (meta != 0 || (compress_params && compress_params->irreversible)) {
        request.flags |= J2K_CODEC_REQUEST_FLAG_LOSSY;
    } else {
        request.flags |= J2K_CODEC_REQUEST_FLAG_LOSSLESS;
    }
    return request;
}

htj2k_codec_request_t make_htj2k_encode_request(uint8_t meta,
                                                blosc2_cparams *cparams,
                                                const void *chunk,
                                                const grk_cparameters *compress_params) {
    htj2k_codec_request_t request = {};
    request.struct_size = sizeof(htj2k_codec_request_t);
    request.meta = meta;
    request.cparams = cparams;
    request.chunk = chunk;
    fill_htj2k_layout(request, cparams);

    if (meta != 0 || (compress_params && compress_params->irreversible)) {
        request.flags |= HTJ2K_CODEC_REQUEST_FLAG_LOSSY;
    } else {
        request.flags |= HTJ2K_CODEC_REQUEST_FLAG_LOSSLESS;
    }
    return request;
}

j2k_codec_request_t make_j2k_decode_request(uint8_t meta,
                                            blosc2_dparams *dparams,
                                            const void *chunk) {
    j2k_codec_request_t request = {};
    request.struct_size = sizeof(j2k_codec_request_t);
    request.meta = meta;
    request.dparams = dparams;
    request.chunk = chunk;
    return request;
}

htj2k_codec_request_t make_htj2k_decode_request(uint8_t meta,
                                                blosc2_dparams *dparams,
                                                const void *chunk) {
    htj2k_codec_request_t request = {};
    request.struct_size = sizeof(htj2k_codec_request_t);
    request.meta = meta;
    request.dparams = dparams;
    request.chunk = chunk;
    return request;
}

}  // namespace blosc2_grok_detail
