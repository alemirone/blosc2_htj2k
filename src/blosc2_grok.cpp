/*********************************************************************
 * blosc2_grok: Grok (JPEG2000 codec) plugin for Blosc2
 *
 * Copyright (c) 2023  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
**********************************************************************/

// General C/C++ helpers used by the codec implementation: debug output,
// environment variables and byte copies.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

// Threading support for guarding Grok's process-global initialization against
// concurrent encoder/decoder use.
#include <mutex>

// RAII buffer ownership used by the native Grok encoder/decoder.
#include <memory>

// Runtime loading support for optional replacement backends.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // LoadLibraryA/GetProcAddress/FreeLibrary.
#else
#include <dlfcn.h>  // dlopen/dlsym/dlclose for backend shared libraries.
#endif

#include "blosc2_grok.h"
#include "blosc2_grok_public.h"

// Minimal C ABI known by the core codec.  Backend-specific details, such as
// Kakadu, stay behind this interface.
#include "j2k_codec_api.h"

#ifdef BLOSC2_MAX_DIM
#define BLOSC2_GROK_MAX_DIM BLOSC2_MAX_DIM
#else
#define BLOSC2_GROK_MAX_DIM B2ND_MAX_DIM
#endif

static grk_cparameters GRK_CPARAMETERS_DEFAULTS = {0};
static bool GRK_INITIALIZED = false;

#if defined(_WIN32)
using plugin_library_handle_t = HMODULE;
#else
using plugin_library_handle_t = void*;
#endif

// Runtime replacement backend state.  `s_replacement_plugin` points to the
// exported J2K_CODEC_PLUGIN descriptor inside the loaded backend library; it can
// describe the reference Grok backend or an optional backend such as Kakadu.
// Two pointers are needed because they have different roles: the plugin pointer
// is the callable ABI object used by the codec, while the handle is the dynamic
// loader ownership token that must be kept for dlclose()/FreeLibrary().  Keeping
// only the ABI pointer would not give us a safe way to unload the library, and
// closing the handle too early would invalidate the ABI pointer.
static j2k_codec_plugin_t* s_replacement_plugin = nullptr;
static plugin_library_handle_t s_plugin_handle = nullptr;

// Function responsibility map:
//
// Strict runtime replacement mechanism:
// - load_replacement_plugin(): discover and cache a backend from BLOSC2_GROK_REPLACEMENT_DIR.
// - replacement_plugin_mutex(): make lazy backend discovery idempotent under concurrent use.
// - has_plugin_library_extension(), open_plugin_library(), close_plugin_library(): isolate platform loader details.
// - find_plugin_descriptor(), is_valid_plugin_descriptor(): resolve and validate the plugin ABI.
// - blosc2_grok_encoder(), blosc2_grok_decoder(): dispatch to the loaded backend, or fall back to native Grok.
// - blosc2_grok_native_encoder(), blosc2_grok_native_decoder(): fallback implementation and target for the reference Grok backend.
// - blosc2_grok_destroy(): release the loaded backend handle and deinitialize Grok.
//
// Native Grok runtime support used by the fallback/reference backend:
// - grok_init_mutex(), ensure_grok_initialized(): guard Grok process-global initialization.
// - blosc2_grok_init(), blosc2_grok_set_default_params(): public runtime setup.
// - beach_decoder(): shared decoder cleanup helper.
namespace {

namespace fs = std::filesystem;

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

// Return the process-wide lock protecting lazy replacement backend loading.
std::mutex &replacement_plugin_mutex() {
    static std::mutex plugin_mutex;
    return plugin_mutex;
}

// Return whether a filesystem entry looks like a loadable backend library.
bool has_plugin_library_extension(const fs::path &path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".so" || ext == ".dylib" || ext == ".dll";
}

#if defined(_WIN32)
// Format the last Windows dynamic-loader error for debug output.
std::string last_windows_loader_error() {
    DWORD error_code = GetLastError();
    if (error_code == 0) {
        return "unknown loader error";
    }

    LPSTR message = nullptr;
    DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                    FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, error_code, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
    if (size == 0 || message == nullptr) {
        return "Windows error " + std::to_string(error_code);
    }

    std::string result(message, size);
    LocalFree(message);
    return result;
}
#endif

// Open a backend shared library with the platform dynamic loader.
plugin_library_handle_t open_plugin_library(const fs::path &libpath, bool debug) {
    std::string path = libpath.string();
#if defined(_WIN32)
    plugin_library_handle_t handle = LoadLibraryA(path.c_str());
    if (!handle && debug) {
        fprintf(stderr, "[blosc2_grok] LoadLibrary failed for %s: %s\n",
                path.c_str(), last_windows_loader_error().c_str());
    }
    return handle;
#else
    plugin_library_handle_t handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!handle && debug) {
        fprintf(stderr, "[blosc2_grok] dlopen failed for %s: %s\n", path.c_str(), dlerror());
    }
    return handle;
#endif
}

// Close a backend shared library handle.
void close_plugin_library(plugin_library_handle_t handle) {
    if (!handle) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

// Resolve the exported plugin descriptor from an open backend library.
j2k_codec_plugin_t* find_plugin_descriptor(plugin_library_handle_t handle,
                                           const fs::path &libpath,
                                           bool debug) {
#if defined(_WIN32)
    FARPROC symbol = GetProcAddress(handle, J2K_CODEC_PLUGIN_SYMBOL);
    if (!symbol) {
        if (debug) {
            std::string path = libpath.string();
            fprintf(stderr, "[blosc2_grok] GetProcAddress failed for %s: %s\n",
                    path.c_str(), last_windows_loader_error().c_str());
        }
        return nullptr;
    }
    return reinterpret_cast<j2k_codec_plugin_t*>(symbol);
#else
    dlerror();
    void *symbol = dlsym(handle, J2K_CODEC_PLUGIN_SYMBOL);
    const char *dlsym_error = dlerror();
    if (dlsym_error) {
        if (debug) {
            std::string path = libpath.string();
            fprintf(stderr, "[blosc2_grok] dlsym failed for %s: %s\n", path.c_str(), dlsym_error);
        }
        return nullptr;
    }
    return reinterpret_cast<j2k_codec_plugin_t*>(symbol);
#endif
}

// Validate the plugin ABI before using names, versions or function pointers.
bool is_valid_plugin_descriptor(const j2k_codec_plugin_t *plugin,
                                const fs::path &libpath,
                                bool debug) {
    std::string path = libpath.string();
    if (!plugin) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Missing plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    if (plugin->abi_version != J2K_CODEC_PLUGIN_ABI_VERSION) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_grok] Plugin ABI mismatch in %s: got %u, expected %u\n",
                    path.c_str(), plugin->abi_version, J2K_CODEC_PLUGIN_ABI_VERSION);
        }
        return false;
    }
    if (plugin->struct_size < sizeof(j2k_codec_plugin_t)) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_grok] Plugin descriptor too small in %s: got %u, expected at least %zu\n",
                    path.c_str(), plugin->struct_size, sizeof(j2k_codec_plugin_t));
        }
        return false;
    }
    if (!plugin->name || !plugin->version || !plugin->vtable.encode || !plugin->vtable.decode) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Incomplete plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    return true;
}

// Load and cache the first valid replacement backend found in BLOSC2_GROK_REPLACEMENT_DIR.
static j2k_codec_plugin_t* load_replacement_plugin(void) {
    std::lock_guard<std::mutex> lock(replacement_plugin_mutex());
    if (s_replacement_plugin) {
        return s_replacement_plugin;
    }

    const char* replacement_dir = getenv("BLOSC2_GROK_REPLACEMENT_DIR");
    if (!replacement_dir || replacement_dir[0] == '\0') {
        return nullptr;
    }

    bool debug = (getenv("BLOSC2_GROK_DEBUG") != nullptr);
    fs::path replacement_path(replacement_dir);
    std::error_code ec;
    if (!fs::is_directory(replacement_path, ec)) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Replacement directory is not readable: %s\n",
                    replacement_dir);
        }
        return nullptr;
    }

    fs::directory_iterator it(replacement_path, ec);
    fs::directory_iterator end;
    if (ec) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Could not scan replacement directory %s: %s\n",
                    replacement_dir, ec.message().c_str());
        }
        return nullptr;
    }

    for (; it != end; it.increment(ec)) {
        if (ec) {
            if (debug) {
                fprintf(stderr, "[blosc2_grok] Error while scanning %s: %s\n",
                        replacement_dir, ec.message().c_str());
            }
            break;
        }

        fs::path libpath = it->path();
        if (!has_plugin_library_extension(libpath)) {
            continue;
        }

        plugin_library_handle_t handle = open_plugin_library(libpath, debug);
        if (!handle) {
            continue;
        }

        j2k_codec_plugin_t *plugin = find_plugin_descriptor(handle, libpath, debug);
        if (is_valid_plugin_descriptor(plugin, libpath, debug)) {
            s_plugin_handle = handle;
            s_replacement_plugin = plugin;
            if (debug) {
                std::string path = libpath.string();
                fprintf(stderr, "[blosc2_grok] Loaded plugin: %s %s from %s\n",
                        plugin->name, plugin->version, path.c_str());
            }
            return plugin;
        }

        close_plugin_library(handle);
    }

    if (debug) {
        fprintf(stderr, "[blosc2_grok] No valid plugin found in %s\n", replacement_dir);
    }
    return nullptr;
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


// Blosc2 encoder entry point: use a replacement backend when configured, otherwise native Grok.
int blosc2_grok_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk
) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    if (debug) {
        fprintf(stderr, "[blosc2_grok] blosc2_grok_encoder called: meta=%d, input_len=%d\n", meta, input_len);
    }

    j2k_codec_plugin_t* plugin = load_replacement_plugin();
    if (plugin) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Using plugin: %s %s\n", plugin->name, plugin->version);
        }
        return plugin->vtable.encode(input, input_len, output, output_len, meta, cparams, chunk);
    }

    return blosc2_grok_native_encoder(input, input_len, output, output_len, meta, cparams, chunk);
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

    ensure_grok_initialized(0, false);

    // Read blosc2 metadata
    uint8_t *content;
    int32_t content_len;
    BLOSC_ERROR(blosc2_meta_get((blosc2_schunk*)cparams->schunk, "b2nd",
                                &content, &content_len));

    int8_t ndim;
    int64_t shape[BLOSC2_GROK_MAX_DIM];
    int32_t chunkshape[BLOSC2_GROK_MAX_DIM];
    int32_t blockshape[BLOSC2_GROK_MAX_DIM];
    char *dtype;
    int8_t dtype_format;
    BLOSC_ERROR(
        b2nd_deserialize_meta(content, content_len, &ndim,
                              shape, chunkshape, blockshape, &dtype, &dtype_format)
    );
    free(content);
    free(dtype);

    // Determine image dimensions
    // Ignore leading dimensions if they are 1
    uint32_t igdim = 0;
    for (int i = 0; i < ndim; ++i) {
        if (blockshape[i] == 1) {
            igdim++;
        } else {
            break;
        }
    }
    // Blosc2 b2nd stores image-like tensors as (..., Y, X[, C]).
    // Map explicitly to codec (X, Y) convention.
    uint32_t dimY = blockshape[igdim];
    uint32_t dimX = blockshape[igdim + 1];
    uint32_t numComps = 1;
    if ((ndim - igdim) == 3) {
        // Single image with more than 1 component
        numComps = blockshape[igdim + 2];
    }

    const uint32_t typesize = ((blosc2_schunk*)cparams->schunk)->typesize;
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

// Blosc2 decoder entry point: use a replacement backend when configured, otherwise native Grok.
int blosc2_grok_decoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                        uint8_t meta, blosc2_dparams *dparams, const void *chunk) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;

    j2k_codec_plugin_t* plugin = load_replacement_plugin();
    if (plugin) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Using plugin decoder: %s %s\n", plugin->name, plugin->version);
        }
        return plugin->vtable.decode(input, input_len, output, output_len, meta, dparams, chunk);
    }

    return blosc2_grok_native_decoder(input, input_len, output, output_len, meta, dparams, chunk);
}

// Native Grok implementation of the Blosc2 decoder entry point.
int blosc2_grok_native_decoder(const uint8_t *input, int32_t input_len, uint8_t *output, int32_t output_len,
                               uint8_t meta, blosc2_dparams *dparams, const void *chunk) {
    (void)meta;
    (void)dparams;
    (void)chunk;

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
    std::lock_guard<std::mutex> lock(replacement_plugin_mutex());
    if (s_plugin_handle) {
        close_plugin_library(s_plugin_handle);
        s_plugin_handle = nullptr;
    }
    s_replacement_plugin = nullptr;
    grk_deinitialize();
}
