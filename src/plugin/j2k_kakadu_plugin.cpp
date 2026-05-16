/*********************************************************************
 * Kakadu replacement backend entry point for blosc2_htj2k.
 *
 * This file only exports the runtime plugin symbol.  The Kakadu implementation
 * lives in kakadu_backend.cpp so the core blosc2_htj2k library remains
 * independent from Kakadu headers and libraries.
 *
 * Copyright (c) 2024  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "j2k_codec_api.h"

// The entry-point file is intentionally small: it binds the generic plugin ABI
// to the Kakadu implementation without exposing Kakadu headers to blosc2_htj2k.
extern "C" int blosc2_kakadu_j2k_supports(const j2k_codec_request_t *request);

extern "C" int blosc2_kakadu_j2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk,
    const j2k_codec_request_t *request
);

extern "C" int blosc2_kakadu_j2k_decoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_dparams *dparams,
    const void *chunk,
    const j2k_codec_request_t *request
);

extern "C" {
J2K_CODEC_PLUGIN_EXPORT j2k_codec_plugin_t J2K_CODEC_PLUGIN = {
    J2K_CODEC_PLUGIN_ABI_VERSION,
    sizeof(j2k_codec_plugin_t),
    "Kakadu",
    "v8.5",
    {
        blosc2_kakadu_j2k_supports,
        blosc2_kakadu_j2k_encoder,
        blosc2_kakadu_j2k_decoder
    },
};
}
