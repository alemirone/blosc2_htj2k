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

// Minimal C ABIs known by the core codec.  Backend-specific details, such as
// Kakadu or OpenHTJ2K, stay behind these interfaces.
#include "htj2k_codec_api.h"
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

// Runtime replacement backend state.  Each codec family has an independent
// descriptor pointer and dynamic-loader handle.
//
// Two pointers are needed for each family because they have different roles:
// the plugin pointer is the callable ABI object used by the codec, while the
// handle is the dynamic-loader ownership token kept for dlclose()/FreeLibrary().
// Keeping only the ABI pointer would not give us a safe way to unload the
// library, and closing the handle too early would invalidate the ABI pointer.
static j2k_codec_plugin_t* s_j2k_replacement_plugin = nullptr;
static plugin_library_handle_t s_j2k_plugin_handle = nullptr;
static htj2k_codec_plugin_t* s_htj2k_replacement_plugin = nullptr;
static plugin_library_handle_t s_htj2k_plugin_handle = nullptr;

// Function responsibility map:
//
// Strict runtime replacement mechanism:
// - load_j2k_replacement_plugin(): discover/cache a J2K backend from BLOSC2_GROK_REPLACEMENT_DIR.
// - load_htj2k_replacement_plugin(): discover/cache an HTJ2K backend from BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR.
// - replacement_plugin_mutex(): make lazy backend discovery idempotent under concurrent use.
// - has_plugin_library_extension(), open_plugin_library(), close_plugin_library(): isolate platform loader details.
// - find_plugin_descriptor(), is_valid_plugin_descriptor(): resolve and validate the plugin ABI.
// - blosc2_grok_encoder(), blosc2_grok_decoder(): route J2K/HTJ2K to the right backend family.
// - blosc2_grok_native_encoder(), blosc2_grok_native_decoder(): J2K fallback implementation and target for the reference Grok backend.
// - blosc2_grok_destroy(): release the loaded backend handle and deinitialize Grok.
//
// JPEG2000-family request shaping and detection:
// - is_htj2k_requested(): detect HTJ2K intent from Grok-compatible parameters.
// - read_b2nd_codec_layout(): extract precision/component metadata for capability checks.
// - make_j2k_*_request(), make_htj2k_*_request(): build plugin-family request structs.
// - detect_codestream_family(): distinguish J2K, HTJ2K and UNKNOWN before decode.
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

// Return whether the current Grok compression parameters request HTJ2K block
// coding.  The core only interprets generic JPEG2000-family intent; backend
// details remain in the plugins.
bool is_htj2k_requested(const grk_cparameters *params) {
    if (!params) {
        return false;
    }
    return ((params->cblk_sty & (GRK_CBLKSTY_HT | GRK_CBLKSTY_HT_MIXED | GRK_CBLKSTY_HT_PHLD)) != 0) ||
	           ((params->rsiz & GRK_JPH_RSIZ_FLAG) != 0);
}

enum class CodecFamily {
    UNKNOWN,
    J2K,
    HTJ2K,
};

// Read big-endian integer fields from JPEG2000 marker segments and boxes.
bool read_be16(const uint8_t *data, int32_t data_len, size_t offset, uint16_t &value) {
    if (data_len < 0 || offset + 2 > static_cast<size_t>(data_len)) {
        return false;
    }
    value = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                  static_cast<uint16_t>(data[offset + 1]));
    return true;
}

// Read a big-endian 32-bit field from a JP2-family box.
bool read_be32(const uint8_t *data, int32_t data_len, size_t offset, uint32_t &value) {
    if (data_len < 0 || offset + 4 > static_cast<size_t>(data_len)) {
        return false;
    }
    value = (static_cast<uint32_t>(data[offset]) << 24) |
            (static_cast<uint32_t>(data[offset + 1]) << 16) |
            (static_cast<uint32_t>(data[offset + 2]) << 8) |
            static_cast<uint32_t>(data[offset + 3]);
    return true;
}

// Read a big-endian 64-bit field from a JP2-family extended-size box.
bool read_be64(const uint8_t *data, int32_t data_len, size_t offset, uint64_t &value) {
    if (data_len < 0 || offset + 8 > static_cast<size_t>(data_len)) {
        return false;
    }
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(data[offset + i]);
    }
    return true;
}

// Return whether the buffer starts with the JP2/JPH signature box.
bool has_jp2_signature(const uint8_t *data, int32_t data_len) {
    static constexpr uint8_t JP2_SIGNATURE[] = {
        0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a
    };
    return data_len >= static_cast<int32_t>(sizeof(JP2_SIGNATURE)) &&
           std::memcmp(data, JP2_SIGNATURE, sizeof(JP2_SIGNATURE)) == 0;
}

// Inspect a raw JPEG2000 codestream header and classify it as J2K or HTJ2K.
// HTJ2K is signalled either by the JPH Rsiz flag in SIZ or by HT block coding
// style in COD.
CodecFamily detect_raw_codestream_family(const uint8_t *data, int32_t data_len) {
    uint16_t marker = 0;
    if (!read_be16(data, data_len, 0, marker) || marker != 0xff4f) {
        return CodecFamily::UNKNOWN;
    }

    bool saw_siz = false;
    bool saw_ht_signal = false;
    size_t pos = 2;
    while (pos + 2 <= static_cast<size_t>(data_len)) {
        if (data[pos] != 0xff) {
            ++pos;
            continue;
        }
        if (!read_be16(data, data_len, pos, marker)) {
            return CodecFamily::UNKNOWN;
        }
        pos += 2;

        if (marker == 0xff93 || marker == 0xffd9) {  // SOD or EOC.
            break;
        }
        if (marker == 0xff4f || marker == 0xff01) {  // SOC or TEM have no length field here.
            continue;
        }

        uint16_t segment_len = 0;
        if (!read_be16(data, data_len, pos, segment_len) || segment_len < 2) {
            return CodecFamily::UNKNOWN;
        }
        if (pos + segment_len > static_cast<size_t>(data_len)) {
            return CodecFamily::UNKNOWN;
        }

        if (marker == 0xff51) {  // SIZ.
            saw_siz = true;
            uint16_t rsiz = 0;
            if (segment_len >= 4 && read_be16(data, data_len, pos + 2, rsiz) &&
                (rsiz & GRK_JPH_RSIZ_FLAG) != 0) {
                saw_ht_signal = true;
            }
        } else if (marker == 0xff52) {  // COD.
            if (segment_len >= 11) {
                const uint8_t cblk_style = data[pos + 10];
                if ((cblk_style & GRK_CBLKSTY_HT) != 0) {
                    saw_ht_signal = true;
                }
            }
        }

        pos += segment_len;
    }

    if (saw_ht_signal) {
        return CodecFamily::HTJ2K;
    }
    return saw_siz ? CodecFamily::J2K : CodecFamily::UNKNOWN;
}

// Find the contiguous codestream box inside a JP2/JPH container and classify
// the codestream stored there.
CodecFamily detect_jp2_container_codestream_family(const uint8_t *data, int32_t data_len) {
    static constexpr uint32_t JP2C_BOX = 0x6a703263u;  // "jp2c".
    size_t pos = 12;  // Skip the signature box already checked by the caller.
    while (pos + 8 <= static_cast<size_t>(data_len)) {
        uint32_t short_length = 0;
        uint32_t box_type = 0;
        if (!read_be32(data, data_len, pos, short_length) ||
            !read_be32(data, data_len, pos + 4, box_type)) {
            return CodecFamily::UNKNOWN;
        }

        uint64_t box_length = short_length;
        size_t header_size = 8;
        if (short_length == 1) {
            if (!read_be64(data, data_len, pos + 8, box_length)) {
                return CodecFamily::UNKNOWN;
            }
            header_size = 16;
        } else if (short_length == 0) {
            box_length = static_cast<uint64_t>(data_len) - pos;
        }

        if (box_length < header_size || pos + box_length > static_cast<size_t>(data_len)) {
            return CodecFamily::UNKNOWN;
        }
        if (box_type == JP2C_BOX) {
            return detect_raw_codestream_family(
                data + pos + header_size,
                static_cast<int32_t>(box_length - header_size));
        }
        pos += static_cast<size_t>(box_length);
    }
    return CodecFamily::UNKNOWN;
}

// Classify an encoded chunk before decode so the core never guesses by trying
// unrelated plugin families in sequence.
CodecFamily detect_codestream_family(const uint8_t *data, int32_t data_len) {
    if (data == nullptr || data_len <= 0) {
        return CodecFamily::UNKNOWN;
    }
    if (has_jp2_signature(data, data_len)) {
        return detect_jp2_container_codestream_family(data, data_len);
    }
    return detect_raw_codestream_family(data, data_len);
}

// Extract the image-like layout from b2nd metadata for plugin capability
// checks.  Backends still deserialize the metadata themselves when they need
// full geometry.
bool read_b2nd_codec_layout(blosc2_cparams *cparams,
                            uint32_t &precision_bits,
                            uint32_t &num_components) {
    precision_bits = 0;
    num_components = 0;
    if (cparams == nullptr || cparams->schunk == nullptr) {
        return false;
    }

    const auto *schunk = (const blosc2_schunk*)cparams->schunk;
    precision_bits = static_cast<uint32_t>(8 * schunk->typesize);

    uint8_t *content = nullptr;
    int32_t content_len = 0;
    if (blosc2_meta_get((blosc2_schunk*)cparams->schunk, "b2nd",
                        &content, &content_len) < 0) {
        return false;
    }

    int8_t ndim = 0;
    int64_t shape[BLOSC2_GROK_MAX_DIM];
    int32_t chunkshape[BLOSC2_GROK_MAX_DIM];
    int32_t blockshape[BLOSC2_GROK_MAX_DIM];
    char *dtype = nullptr;
    int8_t dtype_format = 0;
    int rc = b2nd_deserialize_meta(content, content_len, &ndim,
                                   shape, chunkshape, blockshape,
                                   &dtype, &dtype_format);
    free(content);
    free(dtype);
    if (rc < 0) {
        return false;
    }

    uint32_t igdim = 0;
    for (int i = 0; i < ndim; ++i) {
        if (blockshape[i] == 1) {
            igdim++;
        } else {
            break;
        }
    }
    if ((ndim - igdim) == 3) {
        num_components = static_cast<uint32_t>(blockshape[igdim + 2]);
    } else {
        num_components = 1;
    }
    return true;
}

// Extract common sample layout into a plugin request.
void fill_j2k_layout(j2k_codec_request_t &request, blosc2_cparams *cparams) {
    uint32_t precision_bits = 0;
    uint32_t num_components = 0;
    if (read_b2nd_codec_layout(cparams, precision_bits, num_components)) {
        request.precision_bits = precision_bits;
        request.num_components = num_components;
    } else if (cparams != nullptr && cparams->schunk != nullptr) {
        const auto *schunk = (const blosc2_schunk*)cparams->schunk;
        request.precision_bits = static_cast<uint32_t>(8 * schunk->typesize);
    }
}

// Extract common sample layout into an HTJ2K plugin request.
void fill_htj2k_layout(htj2k_codec_request_t &request, blosc2_cparams *cparams) {
    uint32_t precision_bits = 0;
    uint32_t num_components = 0;
    if (read_b2nd_codec_layout(cparams, precision_bits, num_components)) {
        request.precision_bits = precision_bits;
        request.num_components = num_components;
    } else if (cparams != nullptr && cparams->schunk != nullptr) {
        const auto *schunk = (const blosc2_schunk*)cparams->schunk;
        request.precision_bits = static_cast<uint32_t>(8 * schunk->typesize);
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

// Build the J2K request passed to J2K replacement backends.
j2k_codec_request_t make_j2k_encode_request(uint8_t meta,
                                            blosc2_cparams *cparams,
                                            const void *chunk) {
    j2k_codec_request_t request = {};
    request.struct_size = sizeof(j2k_codec_request_t);
    request.meta = meta;
    request.cparams = cparams;
    request.chunk = chunk;
    fill_j2k_layout(request, cparams);

    grk_cparameters *compress_params = current_compress_params(cparams);
    if (meta != 0 || (compress_params && compress_params->irreversible)) {
        request.flags |= J2K_CODEC_REQUEST_FLAG_LOSSY;
    } else {
        request.flags |= J2K_CODEC_REQUEST_FLAG_LOSSLESS;
    }
    return request;
}

// Build the HTJ2K request passed to HTJ2K replacement backends.
htj2k_codec_request_t make_htj2k_encode_request(uint8_t meta,
                                                blosc2_cparams *cparams,
                                                const void *chunk) {
    htj2k_codec_request_t request = {};
    request.struct_size = sizeof(htj2k_codec_request_t);
    request.meta = meta;
    request.cparams = cparams;
    request.chunk = chunk;
    fill_htj2k_layout(request, cparams);

    grk_cparameters *compress_params = current_compress_params(cparams);
    if (meta != 0 || (compress_params && compress_params->irreversible)) {
        request.flags |= HTJ2K_CODEC_REQUEST_FLAG_LOSSY;
    } else {
        request.flags |= HTJ2K_CODEC_REQUEST_FLAG_LOSSLESS;
    }
    return request;
}

// Build the J2K decode request after codestream-family detection selects J2K.
j2k_codec_request_t make_j2k_decode_request(uint8_t meta,
                                            blosc2_dparams *dparams,
                                            const void *chunk) {
    j2k_codec_request_t request = {};
    request.struct_size = sizeof(j2k_codec_request_t);
    request.meta = meta;
    request.dparams = dparams;
    request.chunk = chunk;
    return request;
}

// Build the HTJ2K decode request after codestream-family detection selects HTJ2K.
htj2k_codec_request_t make_htj2k_decode_request(uint8_t meta,
                                                blosc2_dparams *dparams,
                                                const void *chunk) {
    htj2k_codec_request_t request = {};
    request.struct_size = sizeof(htj2k_codec_request_t);
    request.meta = meta;
    request.dparams = dparams;
    request.chunk = chunk;
    return request;
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

// Resolve the exported J2K plugin descriptor from an open backend library.
j2k_codec_plugin_t* find_j2k_plugin_descriptor(plugin_library_handle_t handle,
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

// Resolve the exported HTJ2K plugin descriptor from an open backend library.
htj2k_codec_plugin_t* find_htj2k_plugin_descriptor(plugin_library_handle_t handle,
                                                   const fs::path &libpath,
                                                   bool debug) {
#if defined(_WIN32)
    FARPROC symbol = GetProcAddress(handle, HTJ2K_CODEC_PLUGIN_SYMBOL);
    if (!symbol) {
        if (debug) {
            std::string path = libpath.string();
            fprintf(stderr, "[blosc2_grok] GetProcAddress failed for %s: %s\n",
                    path.c_str(), last_windows_loader_error().c_str());
        }
        return nullptr;
    }
    return reinterpret_cast<htj2k_codec_plugin_t*>(symbol);
#else
    dlerror();
    void *symbol = dlsym(handle, HTJ2K_CODEC_PLUGIN_SYMBOL);
    const char *dlsym_error = dlerror();
    if (dlsym_error) {
        if (debug) {
            std::string path = libpath.string();
            fprintf(stderr, "[blosc2_grok] dlsym failed for %s: %s\n", path.c_str(), dlsym_error);
        }
        return nullptr;
    }
    return reinterpret_cast<htj2k_codec_plugin_t*>(symbol);
#endif
}

// Validate the J2K plugin ABI before using names, versions or function pointers.
bool is_valid_j2k_plugin_descriptor(const j2k_codec_plugin_t *plugin,
                                    const fs::path &libpath,
                                    bool debug) {
    std::string path = libpath.string();
    if (!plugin) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Missing J2K plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    if (plugin->abi_version != J2K_CODEC_PLUGIN_ABI_VERSION) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_grok] J2K plugin ABI mismatch in %s: got %u, expected %u\n",
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
    if (!plugin->name || !plugin->version || !plugin->vtable.supports ||
        !plugin->vtable.encode || !plugin->vtable.decode) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Incomplete J2K plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    return true;
}

// Validate the HTJ2K plugin ABI before using names, versions or function pointers.
bool is_valid_htj2k_plugin_descriptor(const htj2k_codec_plugin_t *plugin,
                                      const fs::path &libpath,
                                      bool debug) {
    std::string path = libpath.string();
    if (!plugin) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Missing HTJ2K plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    if (plugin->abi_version != HTJ2K_CODEC_PLUGIN_ABI_VERSION) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_grok] HTJ2K plugin ABI mismatch in %s: got %u, expected %u\n",
                    path.c_str(), plugin->abi_version, HTJ2K_CODEC_PLUGIN_ABI_VERSION);
        }
        return false;
    }
    if (plugin->struct_size < sizeof(htj2k_codec_plugin_t)) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_grok] HTJ2K plugin descriptor too small in %s: got %u, expected at least %zu\n",
                    path.c_str(), plugin->struct_size, sizeof(htj2k_codec_plugin_t));
        }
        return false;
    }
    if (!plugin->name || !plugin->version || !plugin->vtable.supports ||
        !plugin->vtable.encode || !plugin->vtable.decode) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Incomplete HTJ2K plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    return true;
}

// Load and cache the first valid J2K backend found in BLOSC2_GROK_REPLACEMENT_DIR.
static j2k_codec_plugin_t* load_j2k_replacement_plugin(void) {
    std::lock_guard<std::mutex> lock(replacement_plugin_mutex());
    if (s_j2k_replacement_plugin) {
        return s_j2k_replacement_plugin;
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
            fprintf(stderr, "[blosc2_grok] J2K replacement directory is not readable: %s\n",
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

        j2k_codec_plugin_t *plugin = find_j2k_plugin_descriptor(handle, libpath, debug);
        if (is_valid_j2k_plugin_descriptor(plugin, libpath, debug)) {
            s_j2k_plugin_handle = handle;
            s_j2k_replacement_plugin = plugin;
            if (debug) {
                std::string path = libpath.string();
                fprintf(stderr, "[blosc2_grok] Loaded J2K plugin: %s %s from %s\n",
                        plugin->name, plugin->version, path.c_str());
            }
            return plugin;
        }

        close_plugin_library(handle);
    }

    if (debug) {
        fprintf(stderr, "[blosc2_grok] No valid J2K plugin found in %s\n", replacement_dir);
    }
    return nullptr;
}

// Load and cache the first valid HTJ2K backend found in BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR.
static htj2k_codec_plugin_t* load_htj2k_replacement_plugin(void) {
    std::lock_guard<std::mutex> lock(replacement_plugin_mutex());
    if (s_htj2k_replacement_plugin) {
        return s_htj2k_replacement_plugin;
    }

    const char* replacement_dir = getenv("BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR");
    if (!replacement_dir || replacement_dir[0] == '\0') {
        return nullptr;
    }

    bool debug = (getenv("BLOSC2_GROK_DEBUG") != nullptr);
    fs::path replacement_path(replacement_dir);
    std::error_code ec;
    if (!fs::is_directory(replacement_path, ec)) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] HTJ2K replacement directory is not readable: %s\n",
                    replacement_dir);
        }
        return nullptr;
    }

    fs::directory_iterator it(replacement_path, ec);
    fs::directory_iterator end;
    if (ec) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Could not scan HTJ2K replacement directory %s: %s\n",
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

        htj2k_codec_plugin_t *plugin = find_htj2k_plugin_descriptor(handle, libpath, debug);
        if (is_valid_htj2k_plugin_descriptor(plugin, libpath, debug)) {
            s_htj2k_plugin_handle = handle;
            s_htj2k_replacement_plugin = plugin;
            if (debug) {
                std::string path = libpath.string();
                fprintf(stderr, "[blosc2_grok] Loaded HTJ2K plugin: %s %s from %s\n",
                        plugin->name, plugin->version, path.c_str());
            }
            return plugin;
        }

        close_plugin_library(handle);
    }

    if (debug) {
        fprintf(stderr, "[blosc2_grok] No valid HTJ2K plugin found in %s\n", replacement_dir);
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
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    if (debug) {
        fprintf(stderr, "[blosc2_grok] blosc2_grok_encoder called: meta=%d, input_len=%d\n", meta, input_len);
    }

    CodecFamily family = requested_encode_family(cparams);
    if (family == CodecFamily::HTJ2K) {
        htj2k_codec_request_t request = make_htj2k_encode_request(meta, cparams, chunk);
        htj2k_codec_plugin_t* plugin = load_htj2k_replacement_plugin();
        if (!plugin) {
            fprintf(stderr,
                    "[blosc2_grok] HTJ2K encoding requires BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR "
                    "pointing to an HTJ2K backend such as Kakadu or OpenHTJ2K\n");
            return -1;
        }
        if (!plugin->vtable.supports(&request)) {
            fprintf(stderr,
                    "[blosc2_grok] HTJ2K plugin %s does not support requested layout "
                    "(precision=%u components=%u)\n",
                    plugin->name, request.precision_bits, request.num_components);
            return -1;
        }
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Using HTJ2K plugin: %s %s\n", plugin->name, plugin->version);
        }
        return plugin->vtable.encode(input, input_len, output, output_len, meta, cparams, chunk, &request);
    }

    j2k_codec_request_t request = make_j2k_encode_request(meta, cparams, chunk);
    j2k_codec_plugin_t* plugin = load_j2k_replacement_plugin();
    if (plugin) {
        if (!plugin->vtable.supports(&request)) {
            fprintf(stderr,
                    "[blosc2_grok] J2K plugin %s does not support requested layout "
                    "(precision=%u components=%u)\n",
                    plugin->name, request.precision_bits, request.num_components);
            return -1;
        }
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Using J2K plugin: %s %s\n", plugin->name, plugin->version);
        }
        return plugin->vtable.encode(input, input_len, output, output_len, meta, cparams, chunk, &request);
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
    if (is_htj2k_requested(compressParams)) {
        fprintf(stderr,
                "[blosc2_grok] Native Grok HTJ2K is not enabled; configure "
                "BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR with an HTJ2K backend\n");
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
        htj2k_codec_plugin_t* plugin = load_htj2k_replacement_plugin();
        if (!plugin) {
            fprintf(stderr,
                    "[blosc2_grok] HTJ2K decoding requires BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR "
                    "pointing to an HTJ2K backend such as Kakadu or OpenHTJ2K\n");
            return -1;
        }
        if (!plugin->vtable.supports(&request)) {
            fprintf(stderr, "[blosc2_grok] HTJ2K plugin %s does not support this decode request\n",
                    plugin->name);
            return -1;
        }
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Using HTJ2K plugin decoder: %s %s\n",
                    plugin->name, plugin->version);
        }
        return plugin->vtable.decode(input, input_len, output, output_len, meta, dparams, chunk, &request);
    }

    j2k_codec_plugin_t* plugin = load_j2k_replacement_plugin();
    if (plugin) {
        j2k_codec_request_t request = make_j2k_decode_request(meta, dparams, chunk);
        if (!plugin->vtable.supports(&request)) {
            fprintf(stderr, "[blosc2_grok] J2K plugin %s does not support this decode request\n",
                    plugin->name);
            return -1;
        }
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Using J2K plugin decoder: %s %s\n",
                    plugin->name, plugin->version);
        }
        return plugin->vtable.decode(input, input_len, output, output_len, meta, dparams, chunk, &request);
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
    if (s_j2k_plugin_handle) {
        close_plugin_library(s_j2k_plugin_handle);
        s_j2k_plugin_handle = nullptr;
    }
    if (s_htj2k_plugin_handle) {
        close_plugin_library(s_htj2k_plugin_handle);
        s_htj2k_plugin_handle = nullptr;
    }
    s_j2k_replacement_plugin = nullptr;
    s_htj2k_replacement_plugin = nullptr;
    grk_deinitialize();
}
