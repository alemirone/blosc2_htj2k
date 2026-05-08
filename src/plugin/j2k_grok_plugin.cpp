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

#include "j2k_codec_api.h"
#include "blosc2_grok.h"
#include "blosc2_grok_public.h"

// Reference backend capability check.  It mirrors the native Grok path and
// deliberately refuses HTJ2K uint16, which must be handled by a backend with
// reliable HTJ2K support.
static int grok_plugin_supports(const j2k_codec_request_t *request) {
    if (request == nullptr) {
        return 0;
    }
    if (request->codec_kind == J2K_CODEC_KIND_HTJ2K && request->precision_bits > 8) {
        return 0;
    }
    return 1;
}

// Reference backend encoder: call the native Grok implementation through the
// same ABI used by external replacement backends.
static int grok_plugin_encode(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk,
    const j2k_codec_request_t* /*request*/
) {
    return blosc2_grok_native_encoder(input, input_len, output, output_len, meta, cparams, chunk);
}

// Reference backend decoder: keep the replacement path testable without any
// optional codec library.
static int grok_plugin_decode(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_dparams *dparams,
    const void *chunk,
    const j2k_codec_request_t* /*request*/
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
        grok_plugin_supports,
        grok_plugin_encode,
        grok_plugin_decode
    },
};
}
