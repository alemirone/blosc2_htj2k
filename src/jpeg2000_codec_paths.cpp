/*********************************************************************
 * blosc2_htj2k: JPEG2000 codec dispatch paths.
 *
 * This file receives already-built requests and already-discovered plugins.  It
 * does not read environment variables or open shared libraries.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "jpeg2000_codec_paths.h"

#include <cstdio>

namespace blosc2_htj2k_detail {

int encode_j2k_with_plugin(const uint8_t *input,
                           int32_t input_len,
                           uint8_t *output,
                           int32_t output_len,
                           uint8_t meta,
                           blosc2_cparams *cparams,
                           const void *chunk,
                           const j2k_codec_request_t &request,
                           j2k_codec_plugin_t *plugin,
                           bool debug) {
    if (plugin) {
        if (!plugin->vtable.supports(&request)) {
            fprintf(stderr,
                    "[blosc2_htj2k] J2K plugin %s does not support requested layout "
                    "(precision=%u components=%u)\n",
                    plugin->name, request.precision_bits, request.num_components);
            return -1;
        }
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] Using J2K plugin: %s %s\n", plugin->name, plugin->version);
        }
        return plugin->vtable.encode(input, input_len, output, output_len, meta, cparams, chunk, &request);
    }

    fprintf(stderr, "[blosc2_htj2k] J2K backend plugin is required\n");
    return -1;
}

int encode_htj2k_with_plugin(const uint8_t *input,
                             int32_t input_len,
                             uint8_t *output,
                             int32_t output_len,
                             uint8_t meta,
                             blosc2_cparams *cparams,
                             const void *chunk,
                             const htj2k_codec_request_t &request,
                             htj2k_codec_plugin_t *plugin,
                             bool debug) {
    if (!plugin) {
        fprintf(stderr,
                "[blosc2_htj2k] HTJ2K encoding requires an HTJ2K backend; configure "
                "BLOSC2_HTJ2K_BACKEND (BLOSC2_HTJ2K_PLUGIN_PATH is optional "
                "for default installs), or set legacy BLOSC2_HTJ2K_REPLACEMENT_DIR\n");
        return -1;
    }
    if (!plugin->vtable.supports(&request)) {
        fprintf(stderr,
                "[blosc2_htj2k] HTJ2K plugin %s does not support requested layout "
                "(precision=%u components=%u)\n",
                plugin->name, request.precision_bits, request.num_components);
        return -1;
    }
    if (debug) {
        fprintf(stderr, "[blosc2_htj2k] Using HTJ2K plugin: %s %s\n", plugin->name, plugin->version);
    }
    return plugin->vtable.encode(input, input_len, output, output_len, meta, cparams, chunk, &request);
}

int decode_j2k_with_plugin(const uint8_t *input,
                           int32_t input_len,
                           uint8_t *output,
                           int32_t output_len,
                           uint8_t meta,
                           blosc2_dparams *dparams,
                           const void *chunk,
                           const j2k_codec_request_t &request,
                           j2k_codec_plugin_t *plugin,
                           bool debug) {
    if (plugin) {
        if (!plugin->vtable.supports(&request)) {
            fprintf(stderr, "[blosc2_htj2k] J2K plugin %s does not support this decode request\n",
                    plugin->name);
            return -1;
        }
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] Using J2K plugin decoder: %s %s\n",
                    plugin->name, plugin->version);
        }
        return plugin->vtable.decode(input, input_len, output, output_len, meta, dparams, chunk, &request);
    }

    fprintf(stderr, "[blosc2_htj2k] J2K backend plugin is required\n");
    return -1;
}

int decode_htj2k_with_plugin(const uint8_t *input,
                             int32_t input_len,
                             uint8_t *output,
                             int32_t output_len,
                             uint8_t meta,
                             blosc2_dparams *dparams,
                             const void *chunk,
                             const htj2k_codec_request_t &request,
                             htj2k_codec_plugin_t *plugin,
                             bool debug) {
    if (!plugin) {
        fprintf(stderr,
                "[blosc2_htj2k] HTJ2K decoding requires an HTJ2K backend; configure "
                "BLOSC2_HTJ2K_BACKEND (BLOSC2_HTJ2K_PLUGIN_PATH is optional "
                "for default installs), or set legacy BLOSC2_HTJ2K_REPLACEMENT_DIR\n");
        return -1;
    }
    if (!plugin->vtable.supports(&request)) {
        fprintf(stderr, "[blosc2_htj2k] HTJ2K plugin %s does not support this decode request\n",
                plugin->name);
        return -1;
    }
    if (debug) {
        fprintf(stderr, "[blosc2_htj2k] Using HTJ2K plugin decoder: %s %s\n",
                plugin->name, plugin->version);
    }
    return plugin->vtable.decode(input, input_len, output, output_len, meta, dparams, chunk, &request);
}

}  // namespace blosc2_htj2k_detail
