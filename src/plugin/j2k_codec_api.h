/*********************************************************************
 * Runtime replacement API for blosc2_grok JPEG2000-family backends.
 *
 * A replacement backend is a shared library that exports a single
 * J2K_CODEC_PLUGIN symbol.  blosc2_grok loads it at runtime when
 * BLOSC2_GROK_REPLACEMENT_DIR points to the directory containing the
 * shared library.  The ABI version and struct size are checked before any
 * function pointer is used, so incompatible plugins are rejected cleanly.
 *
 * Copyright (c) 2024  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#ifndef BLOSC2_GROK_J2K_CODEC_API_H
#define BLOSC2_GROK_J2K_CODEC_API_H

#include <stdint.h>
#include "blosc2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define J2K_CODEC_PLUGIN_ABI_VERSION 2u

typedef enum j2k_codec_kind_t {
    J2K_CODEC_KIND_UNKNOWN = 0,
    J2K_CODEC_KIND_J2K = 1,
    J2K_CODEC_KIND_HTJ2K = 2,
} j2k_codec_kind_t;

#define J2K_CODEC_REQUEST_FLAG_LOSSLESS 0x01u
#define J2K_CODEC_REQUEST_FLAG_LOSSY    0x02u

typedef struct j2k_codec_request_t {
    uint32_t struct_size;
    uint32_t codec_kind;
    uint32_t precision_bits;
    uint32_t num_components;
    uint32_t flags;
    uint8_t meta;
    uint8_t reserved[3];
    blosc2_cparams *cparams;
    blosc2_dparams *dparams;
    const void *chunk;
} j2k_codec_request_t;

typedef struct j2k_codec_vtable {
    int (*supports)(const j2k_codec_request_t *request);

    int (*encode)(const uint8_t *input,
                  int32_t input_len,
                  uint8_t *output,
                  int32_t output_len,
                  uint8_t meta,
                  blosc2_cparams *cparams,
                  const void *chunk,
                  const j2k_codec_request_t *request);

    int (*decode)(const uint8_t *input,
                  int32_t input_len,
                  uint8_t *output,
                  int32_t output_len,
                  uint8_t meta,
                  blosc2_dparams *dparams,
                  const void *chunk,
                  const j2k_codec_request_t *request);
} j2k_codec_vtable;

typedef struct j2k_codec_plugin_t {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *name;
    const char *version;
    j2k_codec_vtable vtable;
} j2k_codec_plugin_t;

#ifdef __cplusplus
}
#endif

#define J2K_CODEC_PLUGIN_SYMBOL "J2K_CODEC_PLUGIN"

#if defined(_WIN32)
#define J2K_CODEC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define J2K_CODEC_PLUGIN_EXPORT
#endif

#endif  // BLOSC2_GROK_J2K_CODEC_API_H
