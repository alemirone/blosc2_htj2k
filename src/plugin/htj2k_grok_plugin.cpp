/*********************************************************************
 * Grok HTJ2K replacement backend entry point for blosc2_htj2k.
 *
 * This backend is always built with the package.  It lets deployments select
 * Grok explicitly through the same HTJ2K plugin ABI as Kakadu and OpenHTJ2K.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "htj2k_codec_api.h"

extern "C" int blosc2_grok_htj2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk,
    const htj2k_codec_request_t *request
);

extern "C" int blosc2_grok_htj2k_decoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_dparams *dparams,
    const void *chunk,
    const htj2k_codec_request_t *request
);

extern "C" int blosc2_grok_htj2k_supports(const htj2k_codec_request_t *request) {
    if (request == nullptr) {
        return 0;
    }
    if (request->precision_bits != 0 &&
        !(request->precision_bits == 8 || request->precision_bits == 16)) {
        return 0;
    }
    if (request->num_components != 0 &&
        !(request->num_components == 1 || request->num_components == 3)) {
        return 0;
    }
    if (request->cparams != nullptr &&
        ((request->flags & HTJ2K_CODEC_REQUEST_FLAG_LOSSY) != 0 || request->meta != 0)) {
        return 0;
    }
    return 1;
}

extern "C" {
HTJ2K_CODEC_PLUGIN_EXPORT htj2k_codec_plugin_t HTJ2K_CODEC_PLUGIN = {
    HTJ2K_CODEC_PLUGIN_ABI_VERSION,
    sizeof(htj2k_codec_plugin_t),
    "Grok",
    "bundled",
    {
        blosc2_grok_htj2k_supports,
        blosc2_grok_htj2k_encoder,
        blosc2_grok_htj2k_decoder
    },
};
}
