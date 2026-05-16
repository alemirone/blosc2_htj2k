/*********************************************************************
 * blosc2_htj2k: request builders for J2K and HTJ2K replacement backends.
 *
 * Responsibilities:
 * - detect HTJ2K intent from Grok-compatible compression parameters;
 * - extract sample precision/component layout from Blosc2 b2nd metadata;
 * - build the family-specific plugin request structs.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#ifndef BLOSC2_HTJ2K_CODEC_REQUESTS_H
#define BLOSC2_HTJ2K_CODEC_REQUESTS_H

#include <cstdint>

#include "blosc2_htj2k.h"
#include "htj2k_codec_api.h"
#include "j2k_codec_api.h"

namespace blosc2_htj2k_detail {

// Return whether Grok-compatible compression parameters request HTJ2K block
// coding.  Backend details remain in family-specific plugins.
bool is_htj2k_requested(const grk_cparameters *params);

// Build the J2K request passed to J2K replacement backends.
j2k_codec_request_t make_j2k_encode_request(uint8_t meta,
                                            blosc2_cparams *cparams,
                                            const void *chunk,
                                            const grk_cparameters *compress_params);

// Build the HTJ2K request passed to HTJ2K replacement backends.
htj2k_codec_request_t make_htj2k_encode_request(uint8_t meta,
                                                blosc2_cparams *cparams,
                                                const void *chunk,
                                                const grk_cparameters *compress_params);

// Build the J2K decode request after codestream-family detection selects J2K.
j2k_codec_request_t make_j2k_decode_request(uint8_t meta,
                                            blosc2_dparams *dparams,
                                            const void *chunk);

// Build the HTJ2K decode request after codestream-family detection selects HTJ2K.
htj2k_codec_request_t make_htj2k_decode_request(uint8_t meta,
                                                blosc2_dparams *dparams,
                                                const void *chunk);

}  // namespace blosc2_htj2k_detail

#endif  // BLOSC2_HTJ2K_CODEC_REQUESTS_H
