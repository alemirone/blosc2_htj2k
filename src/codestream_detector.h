/*********************************************************************
 * blosc2_htj2k: JPEG2000 codestream-family detection helpers.
 *
 * Responsibilities:
 * - expose the small CodecFamily enum used by the core dispatcher;
 * - classify encoded chunks as regular J2K, HTJ2K or UNKNOWN before decode;
 * - keep marker parsing and JP2/JPH box scanning out of the main codec file.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#ifndef BLOSC2_HTJ2K_CODESTREAM_DETECTOR_H
#define BLOSC2_HTJ2K_CODESTREAM_DETECTOR_H

#include <cstdint>

namespace blosc2_htj2k_detail {

enum class CodecFamily {
    UNKNOWN,
    J2K,
    HTJ2K,
};

// Classify an encoded chunk before decode so the core never guesses by trying
// unrelated plugin families in sequence.
CodecFamily detect_codestream_family(const uint8_t *data, int32_t data_len);

}  // namespace blosc2_htj2k_detail

#endif  // BLOSC2_HTJ2K_CODESTREAM_DETECTOR_H
