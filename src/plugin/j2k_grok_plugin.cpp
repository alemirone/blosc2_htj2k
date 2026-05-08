/*********************************************************************
 * Grok replacement backend for blosc2_grok.
 *
 * This backend calls the native Grok encoder/decoder and is installed as a
 * small reference plugin.  It exercises the runtime replacement path without
 * requiring Kakadu.
 *
 * Copyright (c) 2026  Alessandro Mirone
 * License: GNU Affero General Public License v3.0
 **********************************************************************/

#include <cstdlib>
#include <cstring>

#include "j2k_codec_api.h"
#include "blosc2_grok.h"
#include "blosc2_grok_public.h"

static int grok_plugin_encode(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk
) {
    return blosc2_grok_native_encoder(input, input_len, output, output_len, meta, cparams, chunk);
}

static int grok_plugin_decode(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_dparams *dparams,
    const void *chunk
) {
    return blosc2_grok_native_decoder(input, input_len, output, output_len, meta, dparams, chunk);
}

extern "C" {
J2K_CODEC_PLUGIN_EXPORT j2k_codec_plugin_t J2K_CODEC_PLUGIN = {
    J2K_CODEC_PLUGIN_ABI_VERSION,
    sizeof(j2k_codec_plugin_t),
    "grok",
    "0.3.6",
    {
        grok_plugin_encode,
        grok_plugin_decode
    },
};
}
