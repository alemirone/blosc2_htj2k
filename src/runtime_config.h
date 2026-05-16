/*********************************************************************
 * blosc2_htj2k: runtime configuration and plugin candidate discovery.
 *
 * This layer owns user-facing policy: explicit API configuration, legacy
 * environment variables, named backend environment variables and defaults.
 * The dynamic loader still owns dlopen/dlsym validation.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#ifndef BLOSC2_HTJ2K_RUNTIME_CONFIG_H
#define BLOSC2_HTJ2K_RUNTIME_CONFIG_H

#include <filesystem>
#include <string>
#include <vector>

namespace blosc2_htj2k_detail {

enum class PluginFamily {
    J2K,
    HTJ2K,
};

struct PluginCandidate {
    PluginFamily family;
    std::string backend;
    std::filesystem::path path;
    bool native = false;
    bool selected = false;
    bool legacy = false;
    bool direct = false;
};

struct FloatRuntimeConfig {
    bool valid = true;
    bool enabled = false;
    uint32_t quant_bits = 0;
    bool clamp_min_set = false;
    bool clamp_max_set = false;
    double clamp_min = 0.0;
    double clamp_max = 0.0;
    uint32_t nan_policy = 0;
    std::string error;
};

int configure_runtime(const char *plugin_path,
                      const char *j2k_backend,
                      const char *htj2k_backend,
                      uint32_t float_flags,
                      uint32_t float_quant_bits,
                      double float_clamp_min,
                      double float_clamp_max,
                      uint32_t float_nan_policy);

void freeze_runtime_config();

const char *last_runtime_error();

void set_runtime_error(const std::string &message);

std::vector<PluginCandidate> plugin_load_candidates(PluginFamily family);

std::vector<PluginCandidate> plugin_inventory_candidates();

std::string runtime_diagnostics_json();

FloatRuntimeConfig resolved_float_config();

std::string family_name(PluginFamily family);

std::string json_escape(const std::string &value);

}  // namespace blosc2_htj2k_detail

#endif  // BLOSC2_HTJ2K_RUNTIME_CONFIG_H
