/*********************************************************************
 * Runtime replacement API for blosc2_htj2k HTJ2K backends.
 *
 * A replacement backend is a shared library that exports a single
 * HTJ2K_CODEC_PLUGIN symbol.  blosc2_htj2k loads it at runtime when
 * BLOSC2_HTJ2K_HTJ2K_REPLACEMENT_DIR points to the directory containing the
 * shared library.  The ABI version and struct size are checked before any
 * function pointer is used, so incompatible plugins are rejected cleanly.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#ifndef BLOSC2_HTJ2K_HTJ2K_CODEC_API_H
#define BLOSC2_HTJ2K_HTJ2K_CODEC_API_H

#include <stdint.h>
#include "blosc2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HTJ2K_CODEC_PLUGIN_ABI_VERSION 1u

#define HTJ2K_CODEC_REQUEST_FLAG_LOSSLESS 0x01u
#define HTJ2K_CODEC_REQUEST_FLAG_LOSSY    0x02u

/* Per-call capability request passed from blosc2_htj2k to HTJ2K backends.
 *
 * This mirrors the J2K request shape but uses distinct names and a distinct
 * exported symbol, so a backend cannot accidentally be used for the wrong
 * codestream family.
 */
typedef struct htj2k_codec_request_t {
    /* ABI guard: lets newer cores reject older/smaller request layouts. */
    uint32_t struct_size;
    /* 0 when unknown, otherwise the expected sample precision. */
    uint32_t precision_bits;
    /* 0 when unknown, otherwise the expected component count. */
    uint32_t num_components;
    /* Combination of HTJ2K_CODEC_REQUEST_FLAG_* values. */
    uint32_t flags;
    /* Original Blosc2 codec_meta byte.  Non-zero currently means rate target. */
    uint8_t meta;
    uint8_t reserved[3];
    /* Original encode/decode call context.  Only one of cparams/dparams is set. */
    blosc2_cparams *cparams;
    blosc2_dparams *dparams;
    const void *chunk;
} htj2k_codec_request_t;

/* Backend operations exported through the descriptor.
 *
 * supports() must be cheap and side-effect free: the core calls it before
 * encode/decode to fail unsupported precision or component combinations with a
 * clear error.
 */
typedef struct htj2k_codec_vtable {
    int (*supports)(const htj2k_codec_request_t *request);

    int (*encode)(const uint8_t *input,
                  int32_t input_len,
                  uint8_t *output,
                  int32_t output_len,
                  uint8_t meta,
                  blosc2_cparams *cparams,
                  const void *chunk,
                  const htj2k_codec_request_t *request);

    int (*decode)(const uint8_t *input,
                  int32_t input_len,
                  uint8_t *output,
                  int32_t output_len,
                  uint8_t meta,
                  blosc2_dparams *dparams,
                  const void *chunk,
                  const htj2k_codec_request_t *request);
} htj2k_codec_vtable;

/* Single symbol exported by every HTJ2K replacement backend shared library. */
typedef struct htj2k_codec_plugin_t {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *name;
    const char *version;
    htj2k_codec_vtable vtable;
} htj2k_codec_plugin_t;

#ifdef __cplusplus
}
#endif

#define HTJ2K_CODEC_PLUGIN_SYMBOL "HTJ2K_CODEC_PLUGIN"

#if defined(_WIN32)
#define HTJ2K_CODEC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define HTJ2K_CODEC_PLUGIN_EXPORT
#endif

#endif  // BLOSC2_HTJ2K_HTJ2K_CODEC_API_H
