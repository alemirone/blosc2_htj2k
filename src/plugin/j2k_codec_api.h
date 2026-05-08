/*********************************************************************
 * Runtime replacement API for blosc2_grok JPEG2000 backends.
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

#define J2K_CODEC_PLUGIN_ABI_VERSION 1u

typedef struct j2k_codec_vtable {
    int (*encode)(const uint8_t *input,
                  int32_t input_len,
                  uint8_t *output,
                  int32_t output_len,
                  uint8_t meta,
                  blosc2_cparams *cparams,
                  const void *chunk);

    int (*decode)(const uint8_t *input,
                  int32_t input_len,
                  uint8_t *output,
                  int32_t output_len,
                  uint8_t meta,
                  blosc2_dparams *dparams,
                  const void *chunk);
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
