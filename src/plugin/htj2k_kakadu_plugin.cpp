/*********************************************************************
 * Kakadu HTJ2K replacement backend entry point for blosc2_grok.
 *
 * This file only exports the HTJ2K runtime plugin symbol.  The Kakadu
 * implementation lives in kakadu_backend.cpp so the core blosc2_grok library
 * remains independent from Kakadu headers and libraries.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "htj2k_codec_api.h"

// The entry-point file is intentionally small: it binds the HTJ2K plugin ABI
// to the shared Kakadu implementation without exposing Kakadu headers to
// blosc2_grok.
extern "C" int blosc2_kakadu_htj2k_supports(const htj2k_codec_request_t *request);

extern "C" int blosc2_kakadu_htj2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk,
    const htj2k_codec_request_t *request
);

extern "C" int blosc2_kakadu_htj2k_decoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_dparams *dparams,
    const void *chunk,
    const htj2k_codec_request_t *request
);

extern "C" {
HTJ2K_CODEC_PLUGIN_EXPORT htj2k_codec_plugin_t HTJ2K_CODEC_PLUGIN = {
    HTJ2K_CODEC_PLUGIN_ABI_VERSION,
    sizeof(htj2k_codec_plugin_t),
    "Kakadu",
    "v8.5",
    {
        blosc2_kakadu_htj2k_supports,
        blosc2_kakadu_htj2k_encoder,
        blosc2_kakadu_htj2k_decoder
    },
};
}
