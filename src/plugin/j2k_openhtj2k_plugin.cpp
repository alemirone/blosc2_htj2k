/*********************************************************************
 * OpenHTJ2K replacement backend entry point for blosc2_grok.
 *
 * This file only exports the runtime plugin symbol.  The implementation lives
 * in openhtj2k_backend.cpp so the core blosc2_grok library remains independent
 * from OpenHTJ2K headers and libraries.
 *
 * Copyright (c) 2026  Alessandro Mirone
 * License: GNU Affero General Public License v3.0
 *********************************************************************/

#include "j2k_codec_api.h"

extern "C" int blosc2_openhtj2k_supports(const j2k_codec_request_t *request);

extern "C" int blosc2_openhtj2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk,
    const j2k_codec_request_t *request
);

extern "C" int blosc2_openhtj2k_decoder(
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
    "OpenHTJ2K",
    "PR190-or-newer",
    {
        blosc2_openhtj2k_supports,
        blosc2_openhtj2k_encoder,
        blosc2_openhtj2k_decoder
    },
};
}
