/*********************************************************************
 * blosc2_htj2k: runtime replacement plugin discovery and loading.
 *
 * This file owns dynamic-loader state.  It deliberately does not know how to
 * encode or decode: callers receive a validated family-specific ABI descriptor.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "plugin_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "runtime_config.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // LoadLibraryA/GetProcAddress/FreeLibrary.
#else
#include <dlfcn.h>  // dlopen/dlsym/dlclose for backend shared libraries.
#endif

namespace blosc2_htj2k_detail {
namespace {

namespace fs = std::filesystem;

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
    return ext == ".so" || ext == ".dylib" || ext == ".dll" || ext == ".pyd";
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
plugin_library_handle_t open_plugin_library(const fs::path &libpath,
                                            bool debug,
                                            std::string *error = nullptr) {
    std::string path = libpath.string();
#if defined(_WIN32)
    plugin_library_handle_t handle = LoadLibraryA(path.c_str());
    if (!handle) {
        std::string loader_error = last_windows_loader_error();
        if (error) {
            *error = loader_error;
        }
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] LoadLibrary failed for %s: %s\n",
                    path.c_str(), loader_error.c_str());
        }
    }
    return handle;
#else
    plugin_library_handle_t handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!handle) {
        const char *loader_error = dlerror();
        if (error) {
            *error = loader_error ? loader_error : "unknown dlopen error";
        }
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] dlopen failed for %s: %s\n",
                    path.c_str(), loader_error ? loader_error : "unknown dlopen error");
        }
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
            fprintf(stderr, "[blosc2_htj2k] GetProcAddress failed for %s: %s\n",
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
            fprintf(stderr, "[blosc2_htj2k] dlsym failed for %s: %s\n", path.c_str(), dlsym_error);
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
            fprintf(stderr, "[blosc2_htj2k] GetProcAddress failed for %s: %s\n",
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
            fprintf(stderr, "[blosc2_htj2k] dlsym failed for %s: %s\n", path.c_str(), dlsym_error);
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
            fprintf(stderr, "[blosc2_htj2k] Missing J2K plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    if (plugin->abi_version != J2K_CODEC_PLUGIN_ABI_VERSION) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_htj2k] J2K plugin ABI mismatch in %s: got %u, expected %u\n",
                    path.c_str(), plugin->abi_version, J2K_CODEC_PLUGIN_ABI_VERSION);
        }
        return false;
    }
    if (plugin->struct_size < sizeof(j2k_codec_plugin_t)) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_htj2k] Plugin descriptor too small in %s: got %u, expected at least %zu\n",
                    path.c_str(), plugin->struct_size, sizeof(j2k_codec_plugin_t));
        }
        return false;
    }
    if (!plugin->name || !plugin->version || !plugin->vtable.supports ||
        !plugin->vtable.encode || !plugin->vtable.decode) {
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] Incomplete J2K plugin descriptor in %s\n", path.c_str());
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
            fprintf(stderr, "[blosc2_htj2k] Missing HTJ2K plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    if (plugin->abi_version != HTJ2K_CODEC_PLUGIN_ABI_VERSION) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_htj2k] HTJ2K plugin ABI mismatch in %s: got %u, expected %u\n",
                    path.c_str(), plugin->abi_version, HTJ2K_CODEC_PLUGIN_ABI_VERSION);
        }
        return false;
    }
    if (plugin->struct_size < sizeof(htj2k_codec_plugin_t)) {
        if (debug) {
            fprintf(stderr,
                    "[blosc2_htj2k] HTJ2K plugin descriptor too small in %s: got %u, expected at least %zu\n",
                    path.c_str(), plugin->struct_size, sizeof(htj2k_codec_plugin_t));
        }
        return false;
    }
    if (!plugin->name || !plugin->version || !plugin->vtable.supports ||
        !plugin->vtable.encode || !plugin->vtable.decode) {
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] Incomplete HTJ2K plugin descriptor in %s\n", path.c_str());
        }
        return false;
    }
    return true;
}

std::vector<fs::path> candidate_library_paths(const PluginCandidate &candidate) {
    std::vector<fs::path> libraries;

    std::error_code ec;
    if (fs::is_regular_file(candidate.path, ec)) {
        if (has_plugin_library_extension(candidate.path)) {
            libraries.push_back(candidate.path);
        }
        return libraries;
    }
    if (!fs::is_directory(candidate.path, ec)) {
        return libraries;
    }

    for (fs::directory_iterator it(candidate.path, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path libpath = it->path();
        if (it->is_regular_file(ec) && has_plugin_library_extension(libpath)) {
            libraries.push_back(libpath);
        }
    }
    std::sort(libraries.begin(), libraries.end());
    return libraries;
}

struct PluginProbeResult {
    PluginCandidate candidate;
    std::string library;
    bool exists = false;
    bool loadable = false;
    bool abi_valid = false;
    std::string plugin_name;
    std::string plugin_version;
    std::string error;
};

PluginProbeResult probe_candidate(const PluginCandidate &candidate, bool debug) {
    PluginProbeResult result;
    result.candidate = candidate;

    std::error_code ec;
    result.exists = fs::exists(candidate.path, ec);
    if (!result.exists) {
        result.error = "path does not exist";
        return result;
    }

    std::vector<fs::path> libraries = candidate_library_paths(candidate);
    if (libraries.empty()) {
        result.error = "no shared library found";
        return result;
    }

    for (const fs::path &libpath : libraries) {
        result.library = libpath.string();
        std::string loader_error;
        plugin_library_handle_t handle = open_plugin_library(libpath, debug, &loader_error);
        if (!handle) {
            result.error = loader_error.empty() ? "could not load shared library" : loader_error;
            continue;
        }
        result.loadable = true;

        bool valid = false;
        if (candidate.family == PluginFamily::J2K) {
            j2k_codec_plugin_t *plugin = find_j2k_plugin_descriptor(handle, libpath, debug);
            valid = is_valid_j2k_plugin_descriptor(plugin, libpath, debug);
            if (valid) {
                result.plugin_name = plugin->name;
                result.plugin_version = plugin->version;
            }
        } else {
            htj2k_codec_plugin_t *plugin = find_htj2k_plugin_descriptor(handle, libpath, debug);
            valid = is_valid_htj2k_plugin_descriptor(plugin, libpath, debug);
            if (valid) {
                result.plugin_name = plugin->name;
                result.plugin_version = plugin->version;
            }
        }
        close_plugin_library(handle);

        if (valid) {
            result.abi_valid = true;
            result.error.clear();
            return result;
        }
        result.error = "plugin descriptor missing or ABI mismatch";
    }
    return result;
}

bool same_candidate_identity(const PluginCandidate &lhs, const PluginCandidate &rhs) {
    return lhs.family == rhs.family &&
           lhs.backend == rhs.backend &&
           lhs.path == rhs.path;
}

void mark_selected_plugins(std::vector<PluginProbeResult> &results) {
    for (PluginProbeResult &result : results) {
        result.candidate.selected = false;
    }

    for (PluginFamily family : {PluginFamily::J2K, PluginFamily::HTJ2K}) {
        std::vector<PluginCandidate> load_candidates = plugin_load_candidates(family);
        bool selected = false;
        for (const PluginCandidate &load_candidate : load_candidates) {
            for (PluginProbeResult &result : results) {
                if (same_candidate_identity(result.candidate, load_candidate) &&
                    result.loadable && result.abi_valid) {
                    result.candidate.selected = true;
                    selected = true;
                    break;
                }
            }
            if (selected) {
                break;
            }
        }
    }
}

void append_plugin_json(std::ostringstream &out, const PluginProbeResult &result) {
    out << "{";
    out << "\"family\":\"" << family_name(result.candidate.family) << "\",";
    out << "\"backend\":\"" << json_escape(result.candidate.backend) << "\",";
    out << "\"path\":\"" << json_escape(result.candidate.path.string()) << "\",";
    out << "\"library\":\"" << json_escape(result.library) << "\",";
    out << "\"name\":\"" << json_escape(result.plugin_name) << "\",";
    out << "\"version\":\"" << json_escape(result.plugin_version) << "\",";
    out << "\"legacy\":" << (result.candidate.legacy ? "true" : "false") << ",";
    out << "\"direct\":" << (result.candidate.direct ? "true" : "false") << ",";
    out << "\"exists\":" << (result.exists ? "true" : "false") << ",";
    out << "\"loadable\":" << (result.loadable ? "true" : "false") << ",";
    out << "\"abi_valid\":" << (result.abi_valid ? "true" : "false") << ",";
    out << "\"selected\":" << (result.candidate.selected ? "true" : "false") << ",";
    out << "\"error\":\"" << json_escape(result.error) << "\"";
    out << "}";
}

}  // namespace

j2k_codec_plugin_t* load_j2k_replacement_plugin() {
    std::lock_guard<std::mutex> lock(replacement_plugin_mutex());
    if (s_j2k_replacement_plugin) {
        return s_j2k_replacement_plugin;
    }

    std::vector<PluginCandidate> candidates = plugin_load_candidates(PluginFamily::J2K);
    if (candidates.empty()) {
        return nullptr;
    }

    bool debug = (getenv("BLOSC2_HTJ2K_DEBUG") != nullptr);
    for (const PluginCandidate &candidate : candidates) {
        for (const fs::path &libpath : candidate_library_paths(candidate)) {
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
                    fprintf(stderr, "[blosc2_htj2k] Loaded J2K plugin: %s %s from %s\n",
                            plugin->name, plugin->version, path.c_str());
                }
                return plugin;
            }

            close_plugin_library(handle);
        }
    }

    if (debug) {
        fprintf(stderr, "[blosc2_htj2k] No valid J2K plugin found in configured candidates\n");
    }
    return nullptr;
}

htj2k_codec_plugin_t* load_htj2k_replacement_plugin() {
    std::lock_guard<std::mutex> lock(replacement_plugin_mutex());
    if (s_htj2k_replacement_plugin) {
        return s_htj2k_replacement_plugin;
    }

    std::vector<PluginCandidate> candidates = plugin_load_candidates(PluginFamily::HTJ2K);
    if (candidates.empty()) {
        return nullptr;
    }

    bool debug = (getenv("BLOSC2_HTJ2K_DEBUG") != nullptr);
    for (const PluginCandidate &candidate : candidates) {
        for (const fs::path &libpath : candidate_library_paths(candidate)) {
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
                    fprintf(stderr, "[blosc2_htj2k] Loaded HTJ2K plugin: %s %s from %s\n",
                            plugin->name, plugin->version, path.c_str());
                }
                return plugin;
            }

            close_plugin_library(handle);
        }
    }

    if (debug) {
        fprintf(stderr, "[blosc2_htj2k] No valid HTJ2K plugin found in configured candidates\n");
    }
    return nullptr;
}

void unload_replacement_plugins() {
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
}

std::string list_plugins_json() {
    bool debug = (getenv("BLOSC2_HTJ2K_DEBUG") != nullptr);
    std::vector<PluginProbeResult> results;
    for (const PluginCandidate &candidate : plugin_inventory_candidates()) {
        results.push_back(probe_candidate(candidate, debug));
    }
    mark_selected_plugins(results);

    std::ostringstream out;
    out << "{\"plugins\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        append_plugin_json(out, results[i]);
    }
    out << "]}";
    return out.str();
}

std::string diagnose_runtime_json() {
    std::string runtime_json = runtime_diagnostics_json();
    std::string plugins_json = list_plugins_json();

    if (!runtime_json.empty() && runtime_json.back() == '}' &&
        plugins_json.rfind("{\"plugins\":", 0) == 0) {
        return runtime_json.substr(0, runtime_json.size() - 1) + "," +
               plugins_json.substr(1);
    }
    return "{\"runtime\":" + runtime_json + ",\"plugins\":" + plugins_json + "}";
}

}  // namespace blosc2_htj2k_detail
