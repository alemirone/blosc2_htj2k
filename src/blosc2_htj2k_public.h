/*********************************************************************
Blosc - Blocked Shuffling and Compression Library

Copyright (c) 2021  The Blosc Development Team <blosc@blosc.org>
https://blosc.org
License: GNU Affero General Public License v3.0 (see LICENSE.txt)
**********************************************************************/


#ifndef PUBLIC_WRAPPER_H
#define PUBLIC_WRAPPER_H

#if defined(_MSC_VER)
#define BLOSC2_HTJ2K_EXPORT __declspec(dllexport)
#elif (defined(__GNUC__) && __GNUC__ >= 4) || defined(__clang__)
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MINGW32__)
#define BLOSC2_HTJ2K_EXPORT __attribute__((dllexport))
#else
#define BLOSC2_HTJ2K_EXPORT __attribute__((visibility("default")))
#endif  /* defined(_WIN32) || defined(__CYGWIN__) */
#else
#error Cannot determine how to define BLOSC2_HTJ2K_EXPORT for this compiler.
#endif

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#include <stddef.h>
#include "blosc2.h"
#include "blosc2/codecs-registry.h"
#include "grok.h"

#ifndef BLOSC_CODEC_HTJ2K
#define BLOSC_CODEC_HTJ2K 40
#endif
#define BLOSC2_HTJ2K_CODEC_ID BLOSC_CODEC_HTJ2K
#define BLOSC2_HTJ2K_FLOAT_CONFIG_SET     0x01u
#define BLOSC2_HTJ2K_FLOAT_CLAMP_MIN_SET  0x02u
#define BLOSC2_HTJ2K_FLOAT_CLAMP_MAX_SET  0x04u
#define BLOSC2_HTJ2K_FLOAT_NAN_POLICY_FAIL 0u

typedef struct blosc2_htj2k_runtime_config {
    uint32_t struct_size;
    /*
     * Optional plugin search path.  NULL or "" uses the default plugin root
     * installed next to libblosc2_htj2k: <libdir>/plugins.
     */
    const char *plugin_path;
    const char *backend;
    uint32_t float_flags;
    uint32_t float_quant_bits;
    double float_clamp_min;
    double float_clamp_max;
    uint32_t float_nan_policy;
} blosc2_htj2k_runtime_config;


BLOSC2_HTJ2K_EXPORT int blosc2_htj2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk
);

BLOSC2_HTJ2K_EXPORT int blosc2_htj2k_decoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                                          uint8_t meta, blosc2_dparams *dparams, const void *chunk);

BLOSC2_HTJ2K_EXPORT int blosc2_htj2k_native_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk
);

BLOSC2_HTJ2K_EXPORT int blosc2_htj2k_native_decoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                                                 uint8_t meta, blosc2_dparams *dparams, const void *chunk);

BLOSC2_HTJ2K_EXPORT int blosc2_htj2k_configure(const blosc2_htj2k_runtime_config *config);

BLOSC2_HTJ2K_EXPORT int blosc2_htj2k_register_codec(void);

BLOSC2_HTJ2K_EXPORT int blosc2_htj2k_list_plugins(char *buffer, size_t buffer_len);

BLOSC2_HTJ2K_EXPORT int blosc2_htj2k_diagnose(char *buffer, size_t buffer_len);

BLOSC2_HTJ2K_EXPORT const char *blosc2_htj2k_last_error(void);

BLOSC2_HTJ2K_EXPORT codec_info info = {
    .encoder=(char *)"blosc2_htj2k_encoder",
    .decoder=(char *)"blosc2_htj2k_decoder"
};

#if defined(_MSC_VER)
// Needed to export functions in Windows
BLOSC2_HTJ2K_EXPORT void grk_initialize(const char* pluginPath, uint32_t numthreads, bool verbose);
BLOSC2_HTJ2K_EXPORT void grk_deinitialize();
BLOSC2_HTJ2K_EXPORT void grk_object_unref(grk_object* obj);
BLOSC2_HTJ2K_EXPORT grk_image* grk_image_new(uint16_t numcmpts, grk_image_comp* cmptparms,
                                      GRK_COLOR_SPACE clrspc, bool alloc_data);
BLOSC2_HTJ2K_EXPORT void grk_set_default_stream_params(grk_stream_params* params);
BLOSC2_HTJ2K_EXPORT void grk_decompress_set_default_params(grk_decompress_parameters* parameters);
BLOSC2_HTJ2K_EXPORT grk_codec* grk_decompress_init(grk_stream_params* stream_params,
                                            grk_decompress_core_params* core_params);
BLOSC2_HTJ2K_EXPORT bool grk_decompress_read_header(grk_codec* codecWrapper, grk_header_info* header_info);
BLOSC2_HTJ2K_EXPORT grk_image* grk_decompress_get_composited_image(grk_codec* codecWrapper);
BLOSC2_HTJ2K_EXPORT bool grk_decompress(grk_codec* codecWrapper, grk_plugin_tile* tile);
BLOSC2_HTJ2K_EXPORT void grk_compress_set_default_params(grk_cparameters* parameters);
BLOSC2_HTJ2K_EXPORT grk_codec* grk_compress_init(grk_stream_params* stream_params,
                                          grk_cparameters* parameters, grk_image* p_image);
BLOSC2_HTJ2K_EXPORT uint64_t grk_compress(grk_codec* codecWrapper, grk_plugin_tile* tile);
#endif


#ifdef __cplusplus
}
#endif

#endif
