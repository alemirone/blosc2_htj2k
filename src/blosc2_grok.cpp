/*********************************************************************
 * blosc2_grok: Grok (JPEG2000 codec) plugin for Blosc2
 *
 * Copyright (c) 2023  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
**********************************************************************/

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <string>

// Threading support for guarding Grok's process-global initialization against
// concurrent encoder/decoder use.
#include <mutex>

// RAII buffer ownership used by the native Grok encoder/decoder.
#include <memory>

#include "blosc2_grok.h"
#include "b2nd_layout.h"
#include "blosc2_grok_public.h"
#include "codec_requests.h"
#include "codestream_detector.h"
#include "jpeg2000_codec_paths.h"
#include "plugin_loader.h"
#include "runtime_config.h"

static grk_cparameters GRK_CPARAMETERS_DEFAULTS = {0};
static bool GRK_INITIALIZED = false;

using blosc2_grok_detail::B2ndLayout;
using blosc2_grok_detail::CodecFamily;
using blosc2_grok_detail::decode_htj2k_with_plugin;
using blosc2_grok_detail::decode_j2k_with_plugin_or_native;
using blosc2_grok_detail::detect_codestream_family;
using blosc2_grok_detail::encode_htj2k_with_plugin;
using blosc2_grok_detail::encode_j2k_with_plugin_or_native;
using blosc2_grok_detail::image_layout_from_b2nd;
using blosc2_grok_detail::is_htj2k_requested;
using blosc2_grok_detail::load_htj2k_replacement_plugin;
using blosc2_grok_detail::load_j2k_replacement_plugin;
using blosc2_grok_detail::make_htj2k_decode_request;
using blosc2_grok_detail::make_htj2k_encode_request;
using blosc2_grok_detail::make_j2k_decode_request;
using blosc2_grok_detail::make_j2k_encode_request;
using blosc2_grok_detail::read_b2nd_layout;
using blosc2_grok_detail::configure_runtime;
using blosc2_grok_detail::diagnose_runtime_json;
using blosc2_grok_detail::freeze_runtime_config;
using blosc2_grok_detail::last_runtime_error;
using blosc2_grok_detail::list_plugins_json;
using blosc2_grok_detail::set_runtime_error;
using blosc2_grok_detail::unload_replacement_plugins;

// Function responsibility map:
//
// Strict runtime replacement mechanism:
// - plugin_loader.*: discover/cache J2K and HTJ2K backends from their family-specific env vars.
// - jpeg2000_codec_paths.*: call the selected J2K/HTJ2K backend or the native J2K fallback.
// - blosc2_grok_encoder(), blosc2_grok_decoder(): orchestrate family selection and dispatch.
// - blosc2_grok_destroy(): release loaded backend handles and deinitialize Grok.
//
// JPEG2000-family request shaping and detection:
// - codec_requests.*: detect HTJ2K encode intent and build family-specific plugin requests.
// - codestream_detector.*: distinguish J2K, HTJ2K and UNKNOWN before decode.
//
// Native Grok runtime support used by the fallback/reference backend:
// - grok_init_mutex(), ensure_grok_initialized(): guard Grok process-global initialization.
// - blosc2_grok_init(), blosc2_grok_set_default_params(): public runtime setup.
// - beach_decoder(): shared decoder cleanup helper.
namespace {

// Return the process-wide lock protecting Grok initialization.
std::mutex &grok_init_mutex() {
    static std::mutex init_mutex;
    return init_mutex;
}

// Grok keeps process-global initialization state. Guard first-time
// initialization so concurrent encoders cannot initialize it twice.
// Ensure Grok is initialized before a native operation uses Grok APIs.
void ensure_grok_initialized(uint32_t nthreads = 0, bool verbose = false) {
    std::lock_guard<std::mutex> lock(grok_init_mutex());
    if (!GRK_INITIALIZED) {
        blosc2_grok_init(nthreads, verbose);
    }
}

// Return the Grok-compatible parameter block controlling the current encode.
grk_cparameters *current_compress_params(blosc2_cparams *cparams) {
    auto *codec_params = cparams ? (blosc2_grok_params *)cparams->codec_params : nullptr;
    return codec_params ? &codec_params->compressParams : &GRK_CPARAMETERS_DEFAULTS;
}

// Decide whether the current encode asks for regular J2K or HTJ2K.
CodecFamily requested_encode_family(blosc2_cparams *cparams) {
    ensure_grok_initialized(0, false);
    return is_htj2k_requested(current_compress_params(cparams)) ? CodecFamily::HTJ2K
                                                                : CodecFamily::J2K;
}

int copy_string_to_c_buffer(const std::string &value, char *buffer, size_t buffer_len) {
    if (buffer != nullptr && buffer_len > 0) {
        size_t copy_len = (std::min)(value.size(), buffer_len - 1);
        memcpy(buffer, value.data(), copy_len);
        buffer[copy_len] = '\0';
    }
    return static_cast<int>(value.size());
}

}  // namespace


// Initialize Grok and reset the process-wide native Grok compression defaults.
void blosc2_grok_init(uint32_t nthreads, bool verbose) {
    // initialize library
    grk_initialize(nullptr, nthreads, verbose);
    // set default parameters
    grk_compress_set_default_params(&GRK_CPARAMETERS_DEFAULTS);
    GRK_CPARAMETERS_DEFAULTS.cod_format = GRK_FMT_JP2;
    GRK_INITIALIZED = true;
}

// Update the process-wide default Grok compression parameters from Python/C callers.
void blosc2_grok_set_default_params(const int64_t *tile_size, const int64_t *tile_offset,
                                    int numlayers, char *quality_mode, const double *quality_layers,
                                    int numgbits, char *progression,
                                    int num_resolutions, const int64_t *codeblock_size, int cblk_style,
                                    bool irreversible, int roi_compno, int roi_shift, const int64_t *precinct_size,
                                    const int64_t *offset,
                                    GRK_SUPPORTED_FILE_FMT decod_format,
                                    GRK_SUPPORTED_FILE_FMT cod_format, bool enableTilePartGeneration,
                                    int mct, int max_cs_size,
                                    int max_comp_size, int rsiz, int framerate,
                                    bool apply_icc_,
                                    GRK_RATE_CONTROL_ALGORITHM rateControlAlgorithm, int num_threads, int deviceId,
                                    int duration, int repeats,
                                    bool verbose) {
    ensure_grok_initialized(0, false);

    // Change defaults
    if (tile_size[0] == 0 && tile_size[1] == 0) {
        GRK_CPARAMETERS_DEFAULTS.tile_size_on = false;
    } else {
        GRK_CPARAMETERS_DEFAULTS.tile_size_on = true;
    }
    GRK_CPARAMETERS_DEFAULTS.tx0 = tile_offset[0];
    GRK_CPARAMETERS_DEFAULTS.ty0 = tile_offset[1];
    GRK_CPARAMETERS_DEFAULTS.t_width = tile_size[0];
    GRK_CPARAMETERS_DEFAULTS.t_height = tile_size[1];

    GRK_CPARAMETERS_DEFAULTS.numlayers = numlayers;
    // Restore default values
    GRK_CPARAMETERS_DEFAULTS.allocationByRateDistoration = false;
    GRK_CPARAMETERS_DEFAULTS.allocationByQuality = false;
    if (quality_mode != nullptr) {
        if (strcmp(quality_mode, "rates") == 0) {
            GRK_CPARAMETERS_DEFAULTS.allocationByRateDistoration = true;
            for (int i = 0; i < numlayers; ++i) {
                GRK_CPARAMETERS_DEFAULTS.layer_rate[i] = quality_layers[i];
            }
        } else if (strcmp(quality_mode, "dB") == 0) {
            GRK_CPARAMETERS_DEFAULTS.allocationByQuality = true;
            for (int i = 0; i < numlayers; ++i) {
                GRK_CPARAMETERS_DEFAULTS.layer_distortion[i] = quality_layers[i];
            }
        }
    }

    /*for (int i = 0; i < GRK_NUM_COMMENTS_SUPPORTED; ++i) {
        GRK_CPARAMETERS_DEFAULTS.comment[i] = comment[i]; // malloc & memcpy
        GRK_CPARAMETERS_DEFAULTS.comment_len[i] = comment_len[i];
        GRK_CPARAMETERS_DEFAULTS.is_binary_comment[i] = is_binary_comment[i];
    }
    GRK_CPARAMETERS_DEFAULTS.num_comments = num_comments;*/

    // GRK_CPARAMETERS_DEFAULTS.csty = csty;
    GRK_CPARAMETERS_DEFAULTS.numgbits = numgbits;
    if (strcmp(progression, "LRCP") == 0) {
        GRK_CPARAMETERS_DEFAULTS.prog_order = GRK_LRCP;
    } else if (strcmp(progression, "RLCP") == 0) {
        GRK_CPARAMETERS_DEFAULTS.prog_order = GRK_RLCP;
    } else if (strcmp(progression, "RPCL") == 0) {
        GRK_CPARAMETERS_DEFAULTS.prog_order = GRK_RPCL;
    } else if (strcmp(progression, "PCRL") == 0) {
        GRK_CPARAMETERS_DEFAULTS.prog_order = GRK_PCRL;
    } else if (strcmp(progression, "CPRL") == 0) {
        GRK_CPARAMETERS_DEFAULTS.prog_order = GRK_CPRL;
    }

    //for (int i = 0; i < res_spec; ++i) {
    // GRK_CPARAMETERS_DEFAULTS.progression[i] = progression[i];
    // }
    if (precinct_size[0] != 0 && precinct_size[1] != 0) {
        GRK_CPARAMETERS_DEFAULTS.res_spec = 1; // grok can support more than one, but PIL not.
    } else {
        GRK_CPARAMETERS_DEFAULTS.res_spec = 0;
    }
    GRK_CPARAMETERS_DEFAULTS.prcw_init[0] = precinct_size[0];
    GRK_CPARAMETERS_DEFAULTS.prch_init[0] = precinct_size[1];
    // GRK_CPARAMETERS_DEFAULTS.numpocs = numpocs; only one prog supported
    GRK_CPARAMETERS_DEFAULTS.numresolution = num_resolutions;

    GRK_CPARAMETERS_DEFAULTS.cblockw_init = codeblock_size[0];
    GRK_CPARAMETERS_DEFAULTS.cblockh_init = codeblock_size[1];


    GRK_CPARAMETERS_DEFAULTS.irreversible = irreversible;
    GRK_CPARAMETERS_DEFAULTS.roi_compno = roi_compno;
    GRK_CPARAMETERS_DEFAULTS.roi_shift = roi_shift;

    GRK_CPARAMETERS_DEFAULTS.cblk_sty = cblk_style;

    GRK_CPARAMETERS_DEFAULTS.image_offset_x0 = offset[0];
    GRK_CPARAMETERS_DEFAULTS.image_offset_y0 = offset[1];
    // GRK_CPARAMETERS_DEFAULTS.subsampling_dx = subsampling_dx;
    // GRK_CPARAMETERS_DEFAULTS.subsampling_dy = subsampling_dy;

    GRK_CPARAMETERS_DEFAULTS.decod_format = decod_format;
    GRK_CPARAMETERS_DEFAULTS.cod_format = cod_format;
    // GRK_CPARAMETERS_DEFAULTS.raw_cp = raw_cp;
    GRK_CPARAMETERS_DEFAULTS.enableTilePartGeneration = enableTilePartGeneration;
    // GRK_CPARAMETERS_DEFAULTS.newTilePartProgressionDivider = newTilePartProgressionDivider;
    GRK_CPARAMETERS_DEFAULTS.mct = mct;

    // GRK_CPARAMETERS_DEFAULTS.mct_data = mct_data;
    GRK_CPARAMETERS_DEFAULTS.max_cs_size = max_cs_size;

    GRK_CPARAMETERS_DEFAULTS.max_comp_size = max_comp_size;
    GRK_CPARAMETERS_DEFAULTS.rsiz = rsiz;
    GRK_CPARAMETERS_DEFAULTS.framerate = framerate;

    /*for (int i = 0; i < 2; ++i) {
        GRK_CPARAMETERS_DEFAULTS.capture_resolution_from_file[i] = capture_resolution_from_file[i];
        GRK_CPARAMETERS_DEFAULTS.capture_resolution[i] = capture_resolution[i];
        GRK_CPARAMETERS_DEFAULTS.display_resolution[i] = display_resolution[i];
    }
    GRK_CPARAMETERS_DEFAULTS.write_capture_resolution_from_file = write_capture_resolution_from_file;
    GRK_CPARAMETERS_DEFAULTS.write_capture_resolution = write_capture_resolution;
    GRK_CPARAMETERS_DEFAULTS.write_display_resolution = write_display_resolution;*/
    GRK_CPARAMETERS_DEFAULTS.apply_icc_ = apply_icc_;
    GRK_CPARAMETERS_DEFAULTS.rateControlAlgorithm = rateControlAlgorithm;
    GRK_CPARAMETERS_DEFAULTS.numThreads = num_threads;
    GRK_CPARAMETERS_DEFAULTS.deviceId = deviceId;

    GRK_CPARAMETERS_DEFAULTS.duration = duration;
    // GRK_CPARAMETERS_DEFAULTS.kernelBuildOptions = kernelBuildOptions;
    GRK_CPARAMETERS_DEFAULTS.repeats = repeats;
    // GRK_CPARAMETERS_DEFAULTS.writePLT = writePLT;
    // GRK_CPARAMETERS_DEFAULTS.writeTLM = writeTLM;

    GRK_CPARAMETERS_DEFAULTS.verbose = verbose;
    // GRK_CPARAMETERS_DEFAULTS.sharedMemoryInterface = sharedMemoryInterface;

    // Initialize threads and verbose
    grk_initialize(nullptr, GRK_CPARAMETERS_DEFAULTS.numThreads, GRK_CPARAMETERS_DEFAULTS.verbose);
}

int blosc2_grok_configure(const blosc2_grok_runtime_config *config) {
    if (config == nullptr) {
        set_runtime_error("blosc2_grok_configure received a null config");
        return -1;
    }
    if (config->struct_size < sizeof(blosc2_grok_runtime_config)) {
        set_runtime_error("blosc2_grok_runtime_config has an unsupported struct_size");
        return -1;
    }
    return configure_runtime(config->plugin_path, config->j2k_backend, config->htj2k_backend);
}

int blosc2_grok_list_plugins(char *buffer, size_t buffer_len) {
    return copy_string_to_c_buffer(list_plugins_json(), buffer, buffer_len);
}

int blosc2_grok_diagnose(char *buffer, size_t buffer_len) {
    return copy_string_to_c_buffer(diagnose_runtime_json(), buffer, buffer_len);
}

const char *blosc2_grok_last_error(void) {
    return last_runtime_error();
}


// Blosc2 encoder entry point: route J2K to the J2K replacement/native path and
// route HTJ2K exclusively to an HTJ2K replacement plugin.
int blosc2_grok_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk
) {
    freeze_runtime_config();
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    if (debug) {
        fprintf(stderr, "[blosc2_grok] blosc2_grok_encoder called: meta=%d, input_len=%d\n", meta, input_len);
    }

    CodecFamily family = requested_encode_family(cparams);
    grk_cparameters *compress_params = current_compress_params(cparams);
    if (family == CodecFamily::HTJ2K) {
        htj2k_codec_request_t request = make_htj2k_encode_request(meta, cparams, chunk, compress_params);
        return encode_htj2k_with_plugin(input, input_len, output, output_len, meta, cparams, chunk,
                                        request, load_htj2k_replacement_plugin(), debug);
    }

    j2k_codec_request_t request = make_j2k_encode_request(meta, cparams, chunk, compress_params);
    return encode_j2k_with_plugin_or_native(input, input_len, output, output_len, meta, cparams, chunk,
                                            request, load_j2k_replacement_plugin(), debug);
}

// Native Grok implementation of the Blosc2 encoder entry point.
int blosc2_grok_native_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk
) {
    int size = -1;

    freeze_runtime_config();
    ensure_grok_initialized(0, false);

    B2ndLayout layout;
    int64_t dim_x = 0;
    int64_t dim_y = 0;
    int32_t num_comps = 0;
    if (!read_b2nd_layout(cparams, layout) ||
        !image_layout_from_b2nd(layout, dim_x, dim_y, num_comps)) {
        return -1;
    }

    const uint32_t dimX = static_cast<uint32_t>(dim_x);
    const uint32_t dimY = static_cast<uint32_t>(dim_y);
    const uint32_t numComps = static_cast<uint32_t>(num_comps);
    const uint32_t typesize = static_cast<uint32_t>(layout.typesize);
    const uint32_t precision = 8 * typesize;

    // initialize compress parameters
    grk_codec* codec = nullptr;
    auto *codec_params = (blosc2_grok_params *)cparams->codec_params;
    grk_cparameters *compressParams;
    grk_stream_params *streamParams;

    if (codec_params == nullptr) {
        compressParams = &GRK_CPARAMETERS_DEFAULTS;
        streamParams = (grk_stream_params *)malloc(sizeof(grk_stream_params));
        grk_set_default_stream_params(streamParams);
    } else {
        compressParams = &codec_params->compressParams;
        streamParams = &codec_params->streamParams;
    }
    if (is_htj2k_requested(compressParams)) {
        fprintf(stderr,
                "[blosc2_grok] Native Grok HTJ2K is not enabled; configure "
                "BLOSC2_GROK_HTJ2K_BACKEND (BLOSC2_GROK_PLUGIN_PATH is optional "
                "for default installs), or set legacy BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR\n");
        if (codec_params == nullptr) {
            free(streamParams);
        }
        return -1;
    }
    if (meta != 0) {
        // meta indicates we want rates quality mode with meta/10 cratio
        compressParams->allocationByRateDistoration = true;
        compressParams->numlayers = 1;
        compressParams->layer_rate[0] = meta / 10.0;
        if (compressParams->cod_format == GRK_FMT_UNK) {
            compressParams->cod_format = GRK_FMT_JP2;
        }
    }

    std::unique_ptr<uint8_t[]> data;
    size_t bufLen = (size_t)numComps * ((precision + 7) / 8) * dimX * dimY;
    data = std::make_unique<uint8_t[]>(bufLen);
    streamParams->buf = data.get();
    streamParams->buf_len = bufLen;

    // create image from input
    auto* components = new grk_image_comp[numComps];
    for(uint32_t i = 0; i < numComps; ++i) {
        auto c = components + i;
        c->w = dimX;
        c->h = dimY;
        c->dx = 1;
        c->dy = 1;
        c->prec = precision;
        c->sgnd = false;
    }
    grk_image* image;
    if (numComps == 1) {
        image = grk_image_new(
            numComps, components, GRK_CLRSPC_GRAY, true);

    } else {
        image = grk_image_new(
            numComps, components, GRK_CLRSPC_SRGB, true);
    }

    // fill in component data
    // see grok.h header for full details of image structure
    auto *ptr = (uint8_t*)input;
    uint64_t index = 0;
    for (uint16_t compno = 0; compno < image->numcomps; ++compno) {
        index = compno;
        auto comp = image->comps + compno;
        auto compWidth = comp->w;
        auto compHeight = comp->h;
        auto compData = comp->data;
        if (!compData) {
            fprintf(stderr, "Image has null data for component %d\n", compno);
            goto beach;
        }
        // fill in component data, taking component stride into account
        auto srcData = new int32_t[compWidth * compHeight];
        memset(srcData, 0, compWidth * compHeight * sizeof(int32_t));
        for (uint32_t j = 0; j < compHeight; ++j) {
            for (uint32_t i = 0; i < compWidth; ++i) {
                memcpy(srcData + j * compWidth + i, &ptr[index * typesize], typesize);
                index += numComps;
            }
        }

        auto srcPtr = srcData;
        for (uint32_t j = 0; j < compHeight; ++j) {
            memcpy(compData, srcPtr, compWidth * sizeof(int32_t));
            srcPtr += compWidth;
            compData += comp->stride;
        }
        delete[] srcData;
    }

    // initialize compressor
    codec = grk_compress_init(streamParams, compressParams, image);
    if (!codec) {
        fprintf(stderr, "Failed to initialize compressor\n");
        goto beach;
    }

    // compress
    size = (int)grk_compress(codec, nullptr);
    if (size == 0) {
        size = -1;
        fprintf(stderr, "Failed to compress\n");
        goto beach;
    }
    if (size > output_len) {
        // Uncompressible data
        return 0;
    }
    memcpy(output, streamParams->buf, size);

beach:
    // cleanup
    delete[] components;
    grk_object_unref(codec);
    grk_object_unref(&image->obj);
    if (codec_params == nullptr) {
        free(streamParams);
    }

    return size;
}

// Release a partially initialized decoder and return the requested status code.
int beach_decoder(grk_codec * codec, int rc) {
    // cleanup
    grk_object_unref(codec);
    return rc;
}

// Blosc2 decoder entry point: inspect the codestream family first, then route
// to the matching plugin family or native J2K fallback.
int blosc2_grok_decoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                        uint8_t meta, blosc2_dparams *dparams, const void *chunk) {
    freeze_runtime_config();
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;

    CodecFamily family = detect_codestream_family(input, input_len);
    if (family == CodecFamily::UNKNOWN) {
        fprintf(stderr,
                "[blosc2_grok] Could not identify JPEG2000 codestream family; "
                "refusing to guess a decoder backend\n");
        return -1;
    }
    if (family == CodecFamily::HTJ2K) {
        htj2k_codec_request_t request = make_htj2k_decode_request(meta, dparams, chunk);
        return decode_htj2k_with_plugin(input, input_len, output, output_len, meta, dparams, chunk,
                                        request, load_htj2k_replacement_plugin(), debug);
    }

    j2k_codec_request_t request = make_j2k_decode_request(meta, dparams, chunk);
    return decode_j2k_with_plugin_or_native(input, input_len, output, output_len, meta, dparams, chunk,
                                            request, load_j2k_replacement_plugin(), debug);
}

// Native Grok implementation of the Blosc2 decoder entry point.
int blosc2_grok_native_decoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                               uint8_t meta, blosc2_dparams *dparams, const void *chunk) {
    (void)meta;
    (void)dparams;
    (void)chunk;

    freeze_runtime_config();
    ensure_grok_initialized(0, false);

    // initialize decompress parameters
    grk_decompress_parameters decompressParams;
    grk_decompress_set_default_params(&decompressParams);
    decompressParams.compressionLevel = GRK_DECOMPRESS_COMPRESSION_LEVEL_DEFAULT;
    decompressParams.verbose_ = true;

    grk_image *image = nullptr;
    grk_codec *codec = nullptr;

    // initialize decompressor
    grk_stream_params streamParams;
    grk_set_default_stream_params(&streamParams);
    streamParams.buf = (uint8_t *)input;
    streamParams.buf_len = input_len;
    codec = grk_decompress_init(&streamParams, &decompressParams.core);
    if (!codec) {
        fprintf(stderr, "Failed to set up decompressor\n");
        return beach_decoder(codec, BLOSC2_ERROR_FAILURE);
    }

    // read j2k header
    grk_header_info headerInfo;
    memset(&headerInfo, 0, sizeof(headerInfo));
    if (!grk_decompress_read_header(codec, &headerInfo)) {
        fprintf(stderr, "Failed to read the header\n");
        return beach_decoder(codec, BLOSC2_ERROR_FAILURE);
    }

    // retrieve image that will store uncompressed image data
    image = grk_decompress_get_composited_image(codec);
    if (!image) {
        fprintf(stderr, "Failed to retrieve image \n");
        return beach_decoder(codec, BLOSC2_ERROR_FAILURE);
    }

    // decompress all tiles
    if (!grk_decompress(codec, nullptr)){
        fprintf(stderr, "Error when decompressing image\n");
        return beach_decoder(codec, BLOSC2_ERROR_FAILURE);
    }

    // see grok.h header for full details of image structure
    memset(output, 0, output_len);
    auto copyPtr = output;
    uint64_t index = 0;
    for (uint16_t compno = 0; compno < image->numcomps; ++compno) {
        index = compno;
        auto comp = image->comps + compno;
        auto compWidth = comp->w;
        auto compHeight = comp->h;
        auto compData = comp->data;
        if (!compData) {
            fprintf(stderr, "Image has null data for component %d\n", compno);
            return beach_decoder(codec, BLOSC2_ERROR_FAILURE);
        }
        // copy data, taking component stride into account
        int itemsize =  (comp->prec / 8);
        // int itemsize =  ((comp->prec + 7) / 8);
        for (uint32_t j = 0; j < compHeight; ++j) {
            auto compData = comp->data + comp->stride * j;
            for (uint32_t i = 0; i < compWidth; ++i) {
                memcpy(&copyPtr[index * itemsize], compData, itemsize);
                compData += 1;
                index += image->numcomps;
            }
        }
    }

    grk_object_unref(codec);
    return output_len;
}

// Release any loaded replacement backend and deinitialize Grok.
void blosc2_grok_destroy() {
    unload_replacement_plugins();
    grk_deinitialize();
}
