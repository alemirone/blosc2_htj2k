/*********************************************************************
 * blosc2_htj2k: JPEG2000 codec dispatch paths.
 *
 * Responsibilities:
 * - call the selected family-specific plugin;
 * - centralize clear errors for unsupported plugin capabilities.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#ifndef BLOSC2_HTJ2K_JPEG2000_CODEC_PATHS_H
#define BLOSC2_HTJ2K_JPEG2000_CODEC_PATHS_H

#include <cstdint>

#include "htj2k_codec_api.h"
#include "j2k_codec_api.h"

namespace blosc2_htj2k_detail {

// Encode regular J2K through a selected backend plugin.
int encode_j2k_with_plugin(const uint8_t *input,
                           int32_t input_len,
                           uint8_t *output,
                           int32_t output_len,
                           uint8_t meta,
                           blosc2_cparams *cparams,
                           const void *chunk,
                           const j2k_codec_request_t &request,
                           j2k_codec_plugin_t *plugin,
                           bool debug);

// Encode HTJ2K through a replacement plugin.  There is intentionally no native
// Grok fallback for HTJ2K in this MR.
int encode_htj2k_with_plugin(const uint8_t *input,
                             int32_t input_len,
                             uint8_t *output,
                             int32_t output_len,
                             uint8_t meta,
                             blosc2_cparams *cparams,
                             const void *chunk,
                             const htj2k_codec_request_t &request,
                             htj2k_codec_plugin_t *plugin,
                             bool debug);

// Decode regular J2K through a selected backend plugin.
int decode_j2k_with_plugin(const uint8_t *input,
                           int32_t input_len,
                           uint8_t *output,
                           int32_t output_len,
                           uint8_t meta,
                           blosc2_dparams *dparams,
                           const void *chunk,
                           const j2k_codec_request_t &request,
                           j2k_codec_plugin_t *plugin,
                           bool debug);

// Decode HTJ2K through a replacement plugin selected by the HTJ2K loader.
int decode_htj2k_with_plugin(const uint8_t *input,
                             int32_t input_len,
                             uint8_t *output,
                             int32_t output_len,
                             uint8_t meta,
                             blosc2_dparams *dparams,
                             const void *chunk,
                             const htj2k_codec_request_t &request,
                             htj2k_codec_plugin_t *plugin,
                             bool debug);

}  // namespace blosc2_htj2k_detail

#endif  // BLOSC2_HTJ2K_JPEG2000_CODEC_PATHS_H
