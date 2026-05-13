/*********************************************************************
 * blosc2_grok: runtime configuration and plugin candidate discovery.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "runtime_config.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace blosc2_grok_detail {
namespace {

namespace fs = std::filesystem;

struct RuntimeConfig {
    bool explicit_api = false;
    bool frozen = false;
    std::string plugin_path;
    std::string j2k_backend;
    std::string htj2k_backend;
    std::string last_error;
};

struct RuntimeManifest {
    bool exists = false;
    bool parsed = false;
    std::string path;
    std::string error;
    std::string plugin_path;
    std::vector<std::string> j2k_priority;
    std::vector<std::string> htj2k_priority;
};

RuntimeConfig &runtime_config() {
    static RuntimeConfig config;
    return config;
}

std::mutex &runtime_config_mutex() {
    static std::mutex config_mutex;
    return config_mutex;
}

bool is_empty(const char *value) {
    return value == nullptr || value[0] == '\0';
}

bool has_path_separator(const std::string &value) {
    return value.find('/') != std::string::npos ||
           value.find('\\') != std::string::npos;
}

char list_separator() {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

std::vector<std::string> split_path_list(const std::string &value) {
    std::vector<std::string> result;
    std::string current;
    std::istringstream stream(value);
    while (std::getline(stream, current, list_separator())) {
        if (!current.empty()) {
            result.push_back(current);
        }
    }
    return result;
}

std::string env_string(const char *name) {
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string family_dir_name(PluginFamily family) {
    return family == PluginFamily::J2K ? "j2k" : "htj2k";
}

std::string legacy_env_name(PluginFamily family) {
    return family == PluginFamily::J2K ? "BLOSC2_GROK_REPLACEMENT_DIR"
                                       : "BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR";
}

std::string named_backend_env_name(PluginFamily family) {
    return family == PluginFamily::J2K ? "BLOSC2_GROK_J2K_BACKEND"
                                       : "BLOSC2_GROK_HTJ2K_BACKEND";
}

std::string configured_backend_locked(PluginFamily family, const RuntimeConfig &config) {
    std::string api_backend = family == PluginFamily::J2K ? config.j2k_backend : config.htj2k_backend;
    if (config.explicit_api && !api_backend.empty()) {
        return api_backend;
    }

    std::string env_backend = env_string(named_backend_env_name(family).c_str());
    return env_backend;
}

std::string configured_plugin_path_without_manifest_locked(const RuntimeConfig &config) {
    if (config.explicit_api && !config.plugin_path.empty()) {
        return config.plugin_path;
    }
    return env_string("BLOSC2_GROK_PLUGIN_PATH");
}

fs::path self_library_path() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&self_library_path), &module)) {
        char path[MAX_PATH];
        DWORD size = GetModuleFileNameA(module, path, MAX_PATH);
        if (size > 0 && size < MAX_PATH) {
            return fs::path(path);
        }
    }
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&self_library_path), &info) &&
        info.dli_fname != nullptr) {
        return fs::path(info.dli_fname);
    }
#endif
    return {};
}

fs::path self_library_dir() {
    fs::path path = self_library_path();
    return path.empty() ? fs::current_path() : path.parent_path();
}

fs::path default_plugin_root() {
    return self_library_dir() / "plugins";
}

fs::path manifest_path() {
    return self_library_dir() / "blosc2_grok_plugins.json";
}

void skip_json_ws(const std::string &text, size_t &pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t')) {
        ++pos;
    }
}

bool parse_json_string_at(const std::string &text, size_t &pos, std::string &out) {
    skip_json_ws(text, pos);
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < text.size()) {
        char c = text[pos++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (pos >= text.size()) {
                return false;
            }
            char escaped = text[pos++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escaped);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

size_t find_json_key(const std::string &text, const std::string &key, size_t start = 0) {
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = start;
    while ((pos = text.find(quoted_key, pos)) != std::string::npos) {
        size_t after_key = pos + quoted_key.size();
        skip_json_ws(text, after_key);
        if (after_key < text.size() && text[after_key] == ':') {
            return after_key + 1;
        }
        pos += quoted_key.size();
    }
    return std::string::npos;
}

bool parse_json_string_field(const std::string &text, const std::string &key, std::string &value) {
    size_t pos = find_json_key(text, key);
    if (pos == std::string::npos) {
        return false;
    }
    return parse_json_string_at(text, pos, value);
}

bool parse_json_string_array_field(const std::string &text,
                                   const std::string &key,
                                   std::vector<std::string> &values,
                                   std::string &error) {
    size_t pos = find_json_key(text, key);
    if (pos == std::string::npos) {
        return false;
    }
    skip_json_ws(text, pos);
    if (pos >= text.size() || text[pos] != '[') {
        error = "manifest field '" + key + "' is not an array";
        return false;
    }
    ++pos;
    values.clear();
    while (pos < text.size()) {
        skip_json_ws(text, pos);
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            return true;
        }
        std::string value;
        if (!parse_json_string_at(text, pos, value)) {
            error = "manifest field '" + key + "' contains a non-string entry";
            return false;
        }
        if (!value.empty()) {
            values.push_back(value);
        }
        skip_json_ws(text, pos);
        if (pos < text.size() && text[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            return true;
        }
        error = "manifest field '" + key + "' has invalid array syntax";
        return false;
    }
    error = "manifest field '" + key + "' has an unterminated array";
    return false;
}

RuntimeManifest runtime_manifest_locked() {
    RuntimeManifest manifest;
    fs::path path = manifest_path();
    manifest.path = path.string();

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return manifest;
    }
    manifest.exists = true;

    std::ifstream input(path);
    if (!input) {
        manifest.error = "could not open manifest";
        return manifest;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string text = buffer.str();

    parse_json_string_field(text, "plugin_path", manifest.plugin_path);

    std::string parse_error;
    if (!parse_json_string_array_field(text, "j2k", manifest.j2k_priority, parse_error) &&
        !parse_error.empty()) {
        manifest.error = parse_error;
        return manifest;
    }
    parse_error.clear();
    if (!parse_json_string_array_field(text, "htj2k", manifest.htj2k_priority, parse_error) &&
        !parse_error.empty()) {
        manifest.error = parse_error;
        return manifest;
    }

    manifest.parsed = true;
    return manifest;
}

std::string configured_plugin_path_locked(const RuntimeConfig &config) {
    std::string configured_path = configured_plugin_path_without_manifest_locked(config);
    if (!configured_path.empty()) {
        return configured_path;
    }
    RuntimeManifest manifest = runtime_manifest_locked();
    return manifest.parsed ? manifest.plugin_path : std::string();
}

std::vector<fs::path> plugin_roots_locked(const RuntimeConfig &config) {
    std::vector<fs::path> roots;
    std::set<std::string> seen;

    auto add_root = [&](const fs::path &root) {
        if (root.empty()) {
            return;
        }
        std::error_code ec;
        fs::path normalized = fs::absolute(root, ec).lexically_normal();
        if (ec) {
            normalized = root.lexically_normal();
        }
        std::string key = normalized.string();
        if (seen.insert(key).second) {
            roots.push_back(normalized);
        }
    };

    std::string configured_path = configured_plugin_path_locked(config);
    for (const std::string &root : split_path_list(configured_path)) {
        fs::path root_path(root);
        if (!root_path.is_absolute() && configured_plugin_path_without_manifest_locked(config).empty()) {
            root_path = self_library_dir() / root_path;
        }
        add_root(root_path);
    }

    add_root(default_plugin_root());
    return roots;
}

PluginCandidate make_candidate(PluginFamily family,
                               const std::string &backend,
                               const fs::path &path,
                               bool selected,
                               bool legacy,
                               bool direct) {
    PluginCandidate candidate;
    candidate.family = family;
    candidate.backend = backend;
    candidate.path = path;
    candidate.selected = selected;
    candidate.legacy = legacy;
    candidate.direct = direct;
    return candidate;
}

std::vector<std::string> auto_backend_names(PluginFamily family) {
    if (family == PluginFamily::J2K) {
        return {"kakadu", "grok"};
    }
    return {"kakadu", "openhtj2k"};
}

std::vector<PluginCandidate> named_candidates_locked(PluginFamily family,
                                                     const RuntimeConfig &config,
                                                     const std::string &backend,
                                                     bool selected) {
    std::vector<PluginCandidate> candidates;

    if (backend.empty()) {
        return candidates;
    }
    if (family == PluginFamily::J2K && backend == "native") {
        return candidates;
    }

    if (has_path_separator(backend) || fs::path(backend).is_absolute()) {
        fs::path path(backend);
        std::string name = path.filename().string();
        candidates.push_back(make_candidate(family, name.empty() ? backend : name,
                                            path, selected, false, true));
        return candidates;
    }

    std::vector<std::string> backend_names =
        backend == "auto" ? auto_backend_names(family) : std::vector<std::string>{backend};
    for (const fs::path &root : plugin_roots_locked(config)) {
        for (const std::string &name : backend_names) {
            candidates.push_back(make_candidate(
                family, name, root / family_dir_name(family) / name,
                selected, false, false));
        }
    }
    return candidates;
}

PluginCandidate native_j2k_candidate(bool selected) {
    PluginCandidate native;
    native.family = PluginFamily::J2K;
    native.backend = "native";
    native.native = true;
    native.selected = selected;
    return native;
}

void append_unique_candidate(std::vector<PluginCandidate> &candidates,
                             const PluginCandidate &candidate) {
    auto duplicate = std::find_if(candidates.begin(), candidates.end(),
                                  [&](const PluginCandidate &other) {
                                      return other.family == candidate.family &&
                                             other.backend == candidate.backend &&
                                             other.path == candidate.path &&
                                             other.native == candidate.native;
                                  });
    if (duplicate == candidates.end()) {
        candidates.push_back(candidate);
    }
}

std::vector<PluginCandidate> priority_candidates_locked(PluginFamily family,
                                                        const RuntimeConfig &config,
                                                        const std::vector<std::string> &backends,
                                                        bool selected) {
    std::vector<PluginCandidate> candidates;
    for (const std::string &backend : backends) {
        if (backend.empty()) {
            continue;
        }
        if (family == PluginFamily::J2K && backend == "native") {
            append_unique_candidate(candidates, native_j2k_candidate(selected));
            continue;
        }
        for (const PluginCandidate &candidate : named_candidates_locked(family, config, backend, selected)) {
            append_unique_candidate(candidates, candidate);
        }
    }
    return candidates;
}

std::vector<std::string> manifest_priority_locked(PluginFamily family) {
    RuntimeManifest manifest = runtime_manifest_locked();
    if (!manifest.parsed) {
        return {};
    }
    return family == PluginFamily::J2K ? manifest.j2k_priority : manifest.htj2k_priority;
}

std::string basename_or_unknown(const std::string &value) {
    fs::path path(value);
    std::string name = path.filename().string();
    return name.empty() ? "unknown" : name;
}

void append_json_string_field(std::ostringstream &out,
                              const char *name,
                              const std::string &value,
                              bool comma = true) {
    out << "\"" << name << "\":\"" << json_escape(value) << "\"";
    if (comma) {
        out << ",";
    }
}

void append_json_bool_field(std::ostringstream &out,
                            const char *name,
                            bool value,
                            bool comma = true) {
    out << "\"" << name << "\":" << (value ? "true" : "false");
    if (comma) {
        out << ",";
    }
}

void append_env_json_field(std::ostringstream &out, const char *name, bool comma = true) {
    append_json_string_field(out, name, env_string(name), comma);
}

}  // namespace

std::string family_name(PluginFamily family) {
    return family == PluginFamily::J2K ? "j2k" : "htj2k";
}

std::string json_escape(const std::string &value) {
    std::ostringstream out;
    for (char c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        switch (c) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (uc < 0x20) {
                    out << "\\u";
                    const char *hex = "0123456789abcdef";
                    out << "00" << hex[(uc >> 4) & 0xf] << hex[uc & 0xf];
                } else {
                    out << c;
                }
                break;
        }
    }
    return out.str();
}

int configure_runtime(const char *plugin_path,
                      const char *j2k_backend,
                      const char *htj2k_backend) {
    std::lock_guard<std::mutex> lock(runtime_config_mutex());
    RuntimeConfig &config = runtime_config();
    if (config.frozen) {
        config.last_error = "blosc2_grok runtime is already in use; configure before first encode/decode";
        return -1;
    }

    config.explicit_api = true;
    config.plugin_path = is_empty(plugin_path) ? "" : plugin_path;
    config.j2k_backend = is_empty(j2k_backend) ? "" : j2k_backend;
    config.htj2k_backend = is_empty(htj2k_backend) ? "" : htj2k_backend;
    config.last_error.clear();
    return 0;
}

void freeze_runtime_config() {
    std::lock_guard<std::mutex> lock(runtime_config_mutex());
    runtime_config().frozen = true;
}

const char *last_runtime_error() {
    std::lock_guard<std::mutex> lock(runtime_config_mutex());
    static thread_local std::string error_copy;
    error_copy = runtime_config().last_error;
    return error_copy.c_str();
}

void set_runtime_error(const std::string &message) {
    std::lock_guard<std::mutex> lock(runtime_config_mutex());
    runtime_config().last_error = message;
}

std::vector<PluginCandidate> plugin_load_candidates(PluginFamily family) {
    std::lock_guard<std::mutex> lock(runtime_config_mutex());
    const RuntimeConfig &config = runtime_config();

    std::string api_backend = family == PluginFamily::J2K ? config.j2k_backend : config.htj2k_backend;
    if (config.explicit_api && !api_backend.empty()) {
        return named_candidates_locked(family, config, api_backend, true);
    }

    std::string legacy_dir = env_string(legacy_env_name(family).c_str());
    if (!legacy_dir.empty()) {
        return {make_candidate(family, basename_or_unknown(legacy_dir),
                               legacy_dir, true, true, true)};
    }

    std::string env_backend = env_string(named_backend_env_name(family).c_str());
    if (!env_backend.empty()) {
        return named_candidates_locked(family, config, env_backend, true);
    }

    std::vector<std::string> manifest_priority = manifest_priority_locked(family);
    if (!manifest_priority.empty()) {
        return priority_candidates_locked(family, config, manifest_priority, true);
    }

    if (family == PluginFamily::J2K) {
        return {native_j2k_candidate(true)};
    }
    return {};
}

std::vector<PluginCandidate> plugin_inventory_candidates() {
    std::lock_guard<std::mutex> lock(runtime_config_mutex());
    const RuntimeConfig &config = runtime_config();
    std::vector<PluginCandidate> candidates;

    auto add_candidate = [&](const PluginCandidate &candidate) {
        append_unique_candidate(candidates, candidate);
    };

    add_candidate(native_j2k_candidate(false));

    for (PluginFamily family : {PluginFamily::J2K, PluginFamily::HTJ2K}) {
        std::string legacy_dir = env_string(legacy_env_name(family).c_str());
        if (!legacy_dir.empty()) {
            add_candidate(make_candidate(family, basename_or_unknown(legacy_dir),
                                         legacy_dir, false, true, true));
        }

        std::string backend = configured_backend_locked(family, config);
        for (const PluginCandidate &candidate : named_candidates_locked(family, config, backend, false)) {
            add_candidate(candidate);
        }

        for (const PluginCandidate &candidate :
             priority_candidates_locked(family, config, manifest_priority_locked(family), false)) {
            add_candidate(candidate);
        }
    }

    for (const fs::path &root : plugin_roots_locked(config)) {
        for (PluginFamily family : {PluginFamily::J2K, PluginFamily::HTJ2K}) {
            fs::path family_root = root / family_dir_name(family);
            std::error_code ec;
            if (!fs::is_directory(family_root, ec)) {
                continue;
            }
            std::vector<fs::path> entries;
            for (fs::directory_iterator it(family_root, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec) || it->is_regular_file(ec)) {
                    entries.push_back(it->path());
                }
            }
            std::sort(entries.begin(), entries.end());
            for (const fs::path &entry : entries) {
                add_candidate(make_candidate(family, entry.stem().string(), entry, false, false, false));
            }
        }
    }

    return candidates;
}

std::string runtime_diagnostics_json() {
    std::lock_guard<std::mutex> lock(runtime_config_mutex());
    const RuntimeConfig &config = runtime_config();
    std::ostringstream out;
    out << "{";
    append_json_bool_field(out, "explicit_api", config.explicit_api);
    append_json_bool_field(out, "frozen", config.frozen);
    append_json_string_field(out, "library_path", self_library_path().string());
    append_json_string_field(out, "library_dir", self_library_dir().string());
    append_json_string_field(out, "default_plugin_root", default_plugin_root().string());
    RuntimeManifest manifest = runtime_manifest_locked();
    append_json_string_field(out, "manifest_path", manifest.path);
    append_json_bool_field(out, "manifest_exists", manifest.exists);
    append_json_bool_field(out, "manifest_loaded", manifest.parsed);
    append_json_string_field(out, "manifest_error", manifest.error);
    append_json_string_field(out, "manifest_plugin_path", manifest.plugin_path);
    append_json_string_field(out, "plugin_path", configured_plugin_path_locked(config));
    append_json_string_field(out, "j2k_backend", configured_backend_locked(PluginFamily::J2K, config));
    append_json_string_field(out, "htj2k_backend", configured_backend_locked(PluginFamily::HTJ2K, config));
    append_json_string_field(out, "legacy_j2k_dir", env_string("BLOSC2_GROK_REPLACEMENT_DIR"));
    append_json_string_field(out, "legacy_htj2k_dir", env_string("BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR"));
    append_json_string_field(out, "last_error", config.last_error);
    out << "\"manifest_priority\":{\"j2k\":[";
    for (size_t i = 0; i < manifest.j2k_priority.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << "\"" << json_escape(manifest.j2k_priority[i]) << "\"";
    }
    out << "],\"htj2k\":[";
    for (size_t i = 0; i < manifest.htj2k_priority.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << "\"" << json_escape(manifest.htj2k_priority[i]) << "\"";
    }
    out << "]},";
    out << "\"plugin_roots\":[";
    std::vector<fs::path> roots = plugin_roots_locked(config);
    for (size_t i = 0; i < roots.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << "\"" << json_escape(roots[i].string()) << "\"";
    }
    out << "]";
    out << ",\"env\":{";
    append_env_json_field(out, "BLOSC2_GROK_PLUGIN_PATH");
    append_env_json_field(out, "BLOSC2_GROK_J2K_BACKEND");
    append_env_json_field(out, "BLOSC2_GROK_HTJ2K_BACKEND");
    append_env_json_field(out, "BLOSC2_GROK_REPLACEMENT_DIR");
    append_env_json_field(out, "BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR");
    append_env_json_field(out, "BLOSC2_GROK_LIBRARY");
    append_env_json_field(out, "BLOSC2_GROK_DEBUG");
    append_env_json_field(out, "HDF5_PLUGIN_PATH");
    append_env_json_field(out, "LD_LIBRARY_PATH");
    append_env_json_field(out, "DYLD_LIBRARY_PATH");
    append_env_json_field(out, "PATH", false);
    out << "}";
    out << "}";
    return out.str();
}

}  // namespace blosc2_grok_detail
