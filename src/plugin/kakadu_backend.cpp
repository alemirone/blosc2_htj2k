/*********************************************************************
 * blosc2_grok: Kakadu (JPEG2000 codec) backend for Blosc2
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

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

#include "blosc2.h"
#include "b2nd_layout.h"
#include "htj2k_codec_api.h"
#include "j2k_codec_api.h"

#include "kdu_compressed.h"
#include "kdu_messaging.h"
#include "kdu_params.h"
#include "jp2.h"
#include "kdu_file_io.h"
#include "kdu_stripe_compressor.h"
#include "kdu_stripe_decompressor.h"
#include "kdu_threads.h"
#include "kdu_utils.h"

using namespace kdu_core;
using namespace kdu_supp;
using blosc2_grok_detail::B2ndLayout;
using blosc2_grok_detail::image_layout_from_b2nd;
using blosc2_grok_detail::read_b2nd_layout;

// Function responsibility map:
//
// Kakadu process integration:
// - KduErrorHandler/KduWarningHandler/ensure_kakadu_handlers(): route Kakadu diagnostics.
// - ThreadEnvGuard: own optional Kakadu thread environment lifetime.
// - MemTarget: collect Kakadu output bytes in memory for Blosc2 chunks.
//
// Runtime tuning:
// - env_flag(), env_int(), get_kakadu_tune(): parse optional conservative tuning variables.
// - kakadu_extra_has_param(), apply_kakadu_overrides(): apply explicit Kakadu parameter overrides.
// - apply_kakadu_mode(), apply_kakadu_rate_defaults(): choose J2K/HTJ2K and lossless/lossy defaults.
//
// Blosc2 layout adaptation:
// - load_b2nd_info(), set_siz_params(): map b2nd chunk metadata to Kakadu SIZ parameters.
// - has_jp2_signature(): decide whether decode input is JP2 container or raw codestream.
//
// Plugin ABI:
// - blosc2_kakadu_j2k_*(): J2K plugin entry points.
// - blosc2_kakadu_htj2k_*(): HTJ2K plugin entry points.
// - kakadu_encode()/kakadu_decode(): shared implementation used by both physical plugins.
namespace {
class KduErrorHandler : public kdu_message {
  public:
    explicit KduErrorHandler(bool debug_enabled) : debug(debug_enabled) {}
    void put_text(const char *string) override {
        if (string) {
            buffer.append(string);
        }
    }
    void flush(bool end_of_message) override {
        if (end_of_message) {
            if (debug) {
                fprintf(stderr, "[blosc2_grok] Kakadu error: %s\n", buffer.c_str());
            }
            buffer.clear();
            throw KDU_ERROR_EXCEPTION;
        }
    }

  private:
    bool debug = false;
    std::string buffer;
};

class KduWarningHandler : public kdu_message {
  public:
    explicit KduWarningHandler(bool debug_enabled) : debug(debug_enabled) {}
    void put_text(const char *string) override {
        if (string) {
            buffer.append(string);
        }
    }
    void flush(bool end_of_message) override {
        if (end_of_message) {
            if (debug) {
                fprintf(stderr, "[blosc2_grok] Kakadu warning: %s\n", buffer.c_str());
            }
            buffer.clear();
        }
    }

  private:
    bool debug = false;
    std::string buffer;
};

// Install Kakadu message handlers once per process.
void ensure_kakadu_handlers(bool debug) {
    static bool configured = false;
    if (configured) {
        return;
    }
    configured = true;
    static KduErrorHandler err_handler(debug);
    static KduWarningHandler warn_handler(debug);
    kdu_customize_errors(&err_handler);
    kdu_customize_warnings(&warn_handler);
}

// Parse a boolean environment override with a conservative default.
bool env_flag(const char *name, bool default_value) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return default_value;
    }
    auto equals_ci = [](const char *a, const char *b) {
        while (*a != '\0' && *b != '\0') {
            if (std::tolower(static_cast<unsigned char>(*a)) !=
                std::tolower(static_cast<unsigned char>(*b))) {
                return false;
            }
            ++a;
            ++b;
        }
        return *a == '\0' && *b == '\0';
    };
    if (equals_ci(v, "1") || equals_ci(v, "true") || equals_ci(v, "yes") ||
        equals_ci(v, "on")) {
        return true;
    }
    if (equals_ci(v, "0") || equals_ci(v, "false") || equals_ci(v, "no") ||
        equals_ci(v, "off")) {
        return false;
    }
    return default_value;
}

// Parse a bounded integer environment override.
int env_int(const char *name, int default_value) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return default_value;
    }
    char *end = nullptr;
    long n = std::strtol(v, &end, 10);
    if (end == v || (end && *end != '\0')) {
        return default_value;
    }
    if (n < 0) {
        n = 0;
    } else if (n > 1024) {
        n = 1024;
    }
    return static_cast<int>(n);
}

bool set_siz_params(siz_params &siz, int64_t width, int64_t height,
                    int32_t num_comps, int32_t precision);

// Return whether user-provided Kakadu overrides already mention a parameter.
bool kakadu_extra_has_param(const char *param) {
    const char *extra = std::getenv("BLOSC2_GROK_KAKADU_PARAMS");
    if (extra == nullptr || *extra == '\0' || param == nullptr || *param == '\0') {
        return false;
    }
    std::string s(extra);
    std::string p(param);
    for (char &ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    for (char &ch : p) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s.find(p) != std::string::npos;
}

// Set the codestream family: regular JPEG2000 or Part-15 HTJ2K.
void apply_kakadu_mode(siz_params &siz, bool htj2k) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;

    // Apply coding mode. (We ensure canvas/tile coordinates are set explicitly
    // in `set_siz_params`, so Kakadu can safely copy SIZ parameters when other
    // clusters are instantiated by `parse_string` calls below.)
    if (htj2k) {
        siz.parse_string("Scap=P15");
        siz.parse_string("Cmodes=HT");
    } else {
        siz.parse_string("Sncap=P15");
        siz.parse_string("Cmodes=0");
    }
    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu mode: %s\n", htj2k ? "HTJ2K" : "J2K");
    }
}

// Apply optional caller-provided Kakadu parameter strings and Clevels tuning.
void apply_kakadu_overrides(siz_params &siz) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    // Optional: apply extra Kakadu parameter strings (semicolon/newline separated).
    // This is intentionally low-level to allow matching external encoders (e.g., MATLAB)
    // without constantly adding new env vars.
    const char *extra = std::getenv("BLOSC2_GROK_KAKADU_PARAMS");
    if (extra != nullptr && *extra != '\0') {
        std::string s(extra);
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find_first_of(";\n", start);
            if (end == std::string::npos) {
                end = s.size();
            }
            std::string tok = s.substr(start, end - start);
            // trim spaces
            size_t l = tok.find_first_not_of(" \t\r");
            size_t r = tok.find_last_not_of(" \t\r");
            if (l != std::string::npos && r != std::string::npos) {
                tok = tok.substr(l, r - l + 1);
            } else {
                tok.clear();
            }
            if (!tok.empty()) {
                bool ok = siz.parse_string(tok.c_str());
                if (debug) {
                    fprintf(stderr, "[blosc2_grok] Kakadu param: %s (%s)\n",
                            tok.c_str(), ok ? "ok" : "FAILED");
                }
            }
            start = end + 1;
        }
    }

    // Optional wavelet decomposition tuning. We keep the same env var name as the
    // Grok backend so callers can tune both backends uniformly.
    const char *clevels_s = std::getenv("BLOSC2_GROK_CLEVELS");
    if (clevels_s != nullptr && *clevels_s != '\0') {
        char *end = nullptr;
        long clevels = std::strtol(clevels_s, &end, 10);
        if (end != clevels_s && end && *end == '\0') {
            if (clevels < 0) {
                clevels = 0;
            } else if (clevels > 32) {
                clevels = 32;
            }
            std::string cmd = "Clevels=" + std::to_string(clevels);
            bool ok = siz.parse_string(cmd.c_str());
            if (debug) {
                fprintf(stderr, "[blosc2_grok] Kakadu Clevels=%ld (%s)\n",
                        clevels, ok ? "ok" : "FAILED");
            }
        } else if (debug) {
            fprintf(stderr, "[blosc2_grok] Ignoring invalid BLOSC2_GROK_CLEVELS=%s\n", clevels_s);
        }
    }
}

// Choose conservative defaults for lossless/lossy operation unless explicitly
// overridden by BLOSC2_GROK_KAKADU_PARAMS.
void apply_kakadu_rate_defaults(siz_params &siz, int32_t precision, bool rate_controlled) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    if (!rate_controlled) {
        if (!kakadu_extra_has_param("creversible")) {
            bool ok = siz.parse_string("Creversible=yes");
            if (debug) {
                fprintf(stderr, "[blosc2_grok] Kakadu lossless default: Creversible=yes (%s)\n",
                        ok ? "ok" : "FAILED");
            }
        }
        return;
    }
    // Kakadu needs an irreversible transform and a quantization seed to make
    // low target ratios behave like Grok's lossy path unless callers override
    // them.  The final byte budget is still driven by layer_size below.
    //
    // Do not scale the seed all the way down to the nominal 16-bit value.  Very
    // small steps such as 2^-21 produce valid Kakadu streams, but the bundled
    // Grok decoder rejects those 9/7 codestreams.  A 2^-18 floor preserves most
    // of the 16-bit precision while staying on the Grok-readable side.
    if (!kakadu_extra_has_param("creversible")) {
        bool ok = siz.parse_string("Creversible=no");
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu rate default: Creversible=no (%s)\n",
                    ok ? "ok" : "FAILED");
        }
    }
    if (!kakadu_extra_has_param("qstep") && !kakadu_extra_has_param("qfactor")) {
        const double nominal_qstep = std::ldexp(1.0, -(precision + 5));
        const double grok_readable_floor = std::ldexp(1.0, -18);
        const double qstep = (std::max)(nominal_qstep, grok_readable_floor);
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "Qstep=%.17g", qstep);
        bool ok = siz.parse_string(cmd);
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu rate default: %s (%s)\n",
                    cmd, ok ? "ok" : "FAILED");
        }
    }
}

// Identify JP2 containers; otherwise Kakadu decodes the chunk as raw codestream.
bool has_jp2_signature(const uint8_t *data, int32_t len) {
    if (data == nullptr || len < 12) {
        return false;
    }
    static const uint8_t sig[12] = {
        0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a
    };
    return (std::memcmp(data, sig, sizeof(sig)) == 0);
}

struct KakaduTune {
    bool force_precise = true;   // Conservative by default.
    bool want_fastest = false;   // Conservative by default.
    int threads = 0;             // 0/1 => single-thread; >1 => use Kakadu thread env.
};

// Read optional Kakadu speed/precision/thread tuning from the environment.
KakaduTune get_kakadu_tune() {
    KakaduTune t;
    t.force_precise = env_flag("BLOSC2_GROK_KAKADU_PRECISE", true);
    t.want_fastest = env_flag("BLOSC2_GROK_KAKADU_FAST", false);
    t.threads = env_int("BLOSC2_GROK_KAKADU_THREADS", 0);
    return t;
}

// RAII wrapper for Kakadu's optional multi-thread environment.
class ThreadEnvGuard {
  public:
    ThreadEnvGuard() = default;
    ThreadEnvGuard(const ThreadEnvGuard&) = delete;
    ThreadEnvGuard& operator=(const ThreadEnvGuard&) = delete;

    void setup(int threads) {
        if (threads <= 1) {
            return;
        }
        env.create();
        for (int i = 1; i < threads; ++i) {
            env.add_thread();
        }
        created = true;
    }

    kdu_thread_env* ptr() { return created ? &env : nullptr; }

    ~ThreadEnvGuard() {
        if (created) {
            (void) env.destroy();
        }
    }

  private:
    bool created = false;
    kdu_thread_env env;
};

// In-memory Kakadu target used because Blosc2 expects encoded bytes in a caller buffer.
class MemTarget : public kdu_compressed_target_nonnative {
  public:
    std::vector<kdu_byte> data;

    bool post_write(int num_bytes) override {
        size_t offset = data.size();
        data.resize(offset + static_cast<size_t>(num_bytes));
        pull_data(reinterpret_cast<kdu_byte*>(data.data()),
                  static_cast<int>(offset), num_bytes);
        return true;
    }
};

// Read transparent Blosc2 b2nd layout and map (..., Y, X[, C]) to Kakadu X/Y.
bool load_b2nd_info(blosc2_cparams *cparams,
                    int64_t &dim_x, int64_t &dim_y,
                    int32_t &num_comps, int32_t &typesize) {
    B2ndLayout layout;
    if (!read_b2nd_layout(cparams, layout)) {
        return false;
    }
    if (!image_layout_from_b2nd(layout, dim_x, dim_y, num_comps)) {
        return false;
    }
    typesize = layout.typesize;
    return true;
}

// Populate Kakadu SIZ parameters for one full-image tile.
bool set_siz_params(siz_params &siz, int64_t width, int64_t height,
                    int32_t num_comps, int32_t precision) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    siz.set(Scomponents, 0, 0, num_comps);
    siz.set(Ssize, 0, 0, static_cast<int>(height));
    siz.set(Ssize, 0, 1, static_cast<int>(width));
    siz.set(Sorigin, 0, 0, 0);
    siz.set(Sorigin, 0, 1, 0);
    // Make tile/canvas coordinates explicit early, so Kakadu can safely
    // instantiate/copy parameter clusters when we later parse COD/QCD strings.
    // This selects a single tile covering the full image by default.
    siz.set(Stiles, 0, 0, static_cast<int>(height));
    siz.set(Stiles, 0, 1, static_cast<int>(width));
    siz.set(Stile_origin, 0, 0, 0);
    siz.set(Stile_origin, 0, 1, 0);

    for (int c = 0; c < num_comps; ++c) {
        siz.set(Sdims, c, 0, static_cast<int>(height));
        siz.set(Sdims, c, 1, static_cast<int>(width));
        siz.set(Sprecision, c, 0, precision);
        siz.set(Ssigned, c, 0, false);
    }
    return true;
}

}  // namespace

// Shared Kakadu capability check for the transparent J2K/HTJ2K chunk layout.
static int kakadu_supports_layout(uint32_t precision_bits, uint32_t num_components) {
    if (precision_bits != 0 && !(precision_bits == 8 || precision_bits == 16)) {
        return 0;
    }
    if (num_components != 0 && !(num_components == 1 || num_components == 3)) {
        return 0;
    }
    return 1;
}

// Encode one Blosc2 chunk using Kakadu.  The small physical plugin entry point
// chooses whether this shared implementation emits regular J2K or HTJ2K.
int kakadu_encode(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* /*chunk*/,
    bool htj2k
) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    ensure_kakadu_handlers(debug);
    const KakaduTune tune = get_kakadu_tune();
    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu tune: force_precise=%d want_fastest=%d threads=%d\n",
                tune.force_precise ? 1 : 0, tune.want_fastest ? 1 : 0, tune.threads);
    }
    // J2K keeps the existing JP2 container path.  HTJ2K is emitted as a raw
    // Part-15 codestream because this transparent chunk format does not need a
    // JPH file wrapper.
    bool write_jp2 = !htj2k;
    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu encoder path selected (%s, %s)\n",
                htj2k ? "HTJ2K" : "J2K",
                write_jp2 ? "JP2 container" : "raw codestream");
    }
    int64_t dim_x = 0;
    int64_t dim_y = 0;
    int32_t num_comps = 0;
    int32_t typesize = 0;
    bool has_b2nd = load_b2nd_info(cparams, dim_x, dim_y, num_comps, typesize);

    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu has_b2nd=%d dim_x=%ld dim_y=%ld num_comps=%d typesize=%d\n",
                has_b2nd, static_cast<long>(dim_x), static_cast<long>(dim_y), num_comps, typesize);
    }

    // If no b2nd meta available, we cannot proceed - codec_params is not reliable
    if (!has_b2nd) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu encoder error: no b2nd metadata found\n");
        }
        return -1;
    }

    if (!((num_comps == 1) || (num_comps == 3))) {
        return -1;
    }
    if (!(typesize == 1 || typesize == 2)) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu encode error: unsupported typesize=%d (only 1 and 2 are supported)\n",
                    typesize);
        }
        return -1;
    }

    const int precision = typesize * 8;
    const int64_t expected_len = dim_x * dim_y * num_comps * typesize;
    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu encode dims=%ldx%ld comps=%d typesize=%d input_len=%d expected=%ld output_len=%d\n",
                static_cast<long>(dim_x), static_cast<long>(dim_y), num_comps, typesize,
                input_len, static_cast<long>(expected_len), output_len);
    }
    if (input_len < expected_len) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu encode error: input_len too small\n");
        }
        return -1;
    }
    siz_params siz;
    if (!set_siz_params(siz, dim_x, dim_y, num_comps, precision)) {
        return -1;
    }

    MemTarget target;
    kdu_codestream codestream;
    std::unique_ptr<jp2_family_tgt> family;
    std::unique_ptr<jp2_target> jp2;
    try {
        std::unique_ptr<cod_params> cod_holder;
        if (siz.access_cluster(COD_params) == nullptr) {
            cod_holder = std::make_unique<cod_params>();
            cod_holder->link(&siz, -1, -1, 0, 0);
        }
        apply_kakadu_mode(siz, htj2k);
        siz.finalize_all();
        if (write_jp2) {
            family = std::make_unique<jp2_family_tgt>();
            family->open(&target);
            jp2 = std::make_unique<jp2_target>();
            jp2->open(family.get());

            codestream.create(&siz, jp2.get());

            jp2_dimensions dims = jp2->access_dimensions();
            dims.init(&siz, false);
            dims.finalize_compatibility(&siz);

            jp2_colour colour = jp2->access_colour();
            if (num_comps == 1) {
                colour.init(JP2_sLUM_SPACE);
            } else {
                colour.init(JP2_sRGB_SPACE);
            }

            jp2->write_header();
            jp2->open_codestream();
        } else {
            codestream.create(&siz, &target);
        }

        apply_kakadu_rate_defaults(*codestream.access_siz(), precision, meta != 0);
        apply_kakadu_overrides(*codestream.access_siz());
        codestream.access_siz()->finalize_all();

        kdu_stripe_compressor compressor;
        ThreadEnvGuard thread_env;
        thread_env.setup(tune.threads);
        kdu_thread_env *env_ptr = thread_env.ptr();
        if (meta != 0) {
            double rate = meta / 10.0;
            if (rate > 0.0) {
                kdu_long layer_size = static_cast<kdu_long>(
                    static_cast<double>(input_len) / rate);
                compressor.start(codestream, 1, &layer_size, nullptr,
                                 0, false, tune.force_precise, true,
                                 0.0, 0, tune.want_fastest, env_ptr);
            } else {
                compressor.start(codestream, 0, nullptr, nullptr,
                                 0, false, tune.force_precise, true,
                                 0.0, 0, tune.want_fastest, env_ptr);
            }
        } else {
            compressor.start(codestream, 0, nullptr, nullptr,
                             0, false, tune.force_precise, true,
                             0.0, 0, tune.want_fastest, env_ptr);
        }

        std::vector<int> stripe_heights(num_comps, static_cast<int>(dim_y));
        std::vector<int> sample_gaps(num_comps, num_comps);
        std::vector<int> row_gaps(num_comps, static_cast<int>(dim_x) * num_comps);
        std::vector<int> precisions(num_comps, precision);

        bool needs_more = false;
        if (typesize == 1) {
            std::vector<kdu_byte*> stripe_bufs(num_comps);
            for (int c = 0; c < num_comps; ++c) {
                stripe_bufs[c] = const_cast<kdu_byte*>(input) + c;
            }
            needs_more = compressor.push_stripe(stripe_bufs.data(), stripe_heights.data(),
                                                sample_gaps.data(), row_gaps.data(),
                                                precisions.data());
        } else if (typesize == 2) {
            std::vector<kdu_int16*> stripe_bufs(num_comps);
            auto *input16 = reinterpret_cast<const kdu_uint16*>(input);
            for (int c = 0; c < num_comps; ++c) {
                // Kakadu's 16-bit push_stripe overload is typed as
                // kdu_int16*, but with is_signed=false the buffer is
                // explicitly interpreted as unsigned 16-bit input.
                stripe_bufs[c] =
                    reinterpret_cast<kdu_int16*>(const_cast<kdu_uint16*>(input16 + c));
            }
            std::unique_ptr<bool[]> is_signed(new bool[num_comps]);
            for (int c = 0; c < num_comps; ++c) {
                is_signed[c] = false;
            }
            needs_more = compressor.push_stripe(stripe_bufs.data(), stripe_heights.data(),
                                                sample_gaps.data(), row_gaps.data(),
                                                precisions.data(), is_signed.get());
        }
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu push_stripe needs_more=%d\n", needs_more ? 1 : 0);
        }

        compressor.finish();
        codestream.destroy();
        if (jp2) {
            jp2->close();
        }
        if (family) {
            family->close();
        }
    } catch (kdu_exception) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu encoder exception\n");
        }
        return -1;
    }

    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu encoded bytes=%zu\n", target.data.size());
    }
    if (static_cast<int32_t>(target.data.size()) > output_len) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu encode error: output buffer too small\n");
        }
        return 0;
    }
    std::memcpy(output, target.data.data(), target.data.size());
    return static_cast<int>(target.data.size());
}

// Decode one Kakadu-supported JP2 or raw codestream into a Blosc2 chunk buffer.
int kakadu_decode(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t /*meta*/,
    blosc2_dparams * /*dparams*/,
    const void * /*chunk*/
) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    ensure_kakadu_handlers(debug);
    const KakaduTune tune = get_kakadu_tune();
    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu tune: force_precise=%d want_fastest=%d threads=%d\n",
                tune.force_precise ? 1 : 0, tune.want_fastest ? 1 : 0, tune.threads);
    }
    const bool is_jp2 = has_jp2_signature(input, input_len);
    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu decoder path selected (%s)\n",
                is_jp2 ? "JP2 container" : "raw codestream");
    }
    kdu_compressed_source_buffered source;
    source.open(const_cast<kdu_byte*>(reinterpret_cast<const kdu_byte*>(input)),
                static_cast<size_t>(input_len));

    kdu_codestream codestream;
    jp2_family_src family;
    jp2_source jp2;
    try {
        if (is_jp2) {
            family.open(&source);
            jp2.open(&family);
            int hdr = jp2.read_header(true);
            if (hdr <= 0) {
                if (debug) {
                    fprintf(stderr, "[blosc2_grok] Kakadu JP2 header not ready or incompatible\n");
                }
                return -1;
            }
            codestream.create(&jp2);
        } else {
            codestream.create(&source);
        }

        int num_comps = codestream.get_num_components(true);
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu decoder: num_comps=%d\n", num_comps);
        }
        if (!((num_comps == 1) || (num_comps == 3))) {
            codestream.destroy();
            return -1;
        }

        kdu_dims dims;
        codestream.get_dims(0, dims, true);
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu decoder: dims.x=%ld, dims.y=%ld\n", (long)dims.size.x, (long)dims.size.y);
        }
        int width = dims.size.x;
        int height = dims.size.y;
        for (int c = 1; c < num_comps; ++c) {
            kdu_dims cdims;
            codestream.get_dims(c, cdims, true);
            if (cdims.size.x != width || cdims.size.y != height) {
                codestream.destroy();
                return -1;
            }
        }

        int precision = codestream.get_bit_depth(0, true);
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu decoder: codestream precision=%d bits\n", precision);
        }
        // The transparent codec path currently maps Blosc chunks back to uint8/uint16.
        if (!(precision == 8 || precision == 16)) {
            if (debug) {
                fprintf(stderr, "[blosc2_grok] Kakadu: unsupported precision=%d (only 8 and 16 supported)\n", precision);
            }
            codestream.destroy();
            return -1;
        }

        int typesize = (precision <= 8) ? 1 : 2;
        int64_t expected = static_cast<int64_t>(width) *
                           static_cast<int64_t>(height) *
                           static_cast<int64_t>(num_comps) *
                           static_cast<int64_t>(typesize);
        if (expected > output_len) {
            codestream.destroy();
            return -1;
        }

        // Kakadu 8.5 can still touch decoder internals from the
        // kdu_stripe_decompressor destructor after a successful HTJ2K raw
        // codestream reset.  Keep one decoder object per thread and reset it
        // after every chunk; this releases the codestream state without
        // running the destructor on the hot HDF5 filter path.
        static thread_local kdu_stripe_decompressor *tls_decompressor =
            new kdu_stripe_decompressor();
        kdu_stripe_decompressor &decompressor = *tls_decompressor;
        ThreadEnvGuard thread_env;
        thread_env.setup(tune.threads);
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu decoder: starting stripe decompressor\n");
        }
        decompressor.start(codestream, tune.force_precise, tune.want_fastest, thread_env.ptr());

        std::vector<int> stripe_heights(num_comps, 0);
        std::vector<int> precisions(num_comps, precision);
        const int requested_stripe_height = env_int("BLOSC2_GROK_KAKADU_DECODE_STRIPE_HEIGHT", 128);
        const int max_stripe_height =
            (requested_stripe_height > 0 && requested_stripe_height < height) ?
            requested_stripe_height : height;

        // Blosc2 chunks are one contiguous sample-interleaved buffer.  Kakadu
        // has a dedicated single-buffer overload for this layout; passing an
        // array of per-component pointers is more fragile for the full-image
        // stripe.  Pulling moderate stripes also keeps Kakadu cleanup simple
        // for large, real detector frames.
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu decoder: pulling stripes height<=%d\n",
                    max_stripe_height);
        }
        std::unique_ptr<bool[]> is_signed;
        if (typesize == 2) {
            is_signed.reset(new bool[num_comps]);
            for (int c = 0; c < num_comps; ++c) {
                is_signed[c] = false;
            }
        }
        std::vector<uint8_t> decoded8;
        std::vector<kdu_uint16> decoded16;
        if (typesize == 1) {
            decoded8.resize(static_cast<size_t>(expected));
        } else if (typesize == 2) {
            decoded16.resize(static_cast<size_t>(expected / typesize));
        }

        int rows_done = 0;
        bool needs_more = true;
        while (rows_done < height) {
            int rows = height - rows_done;
            if (rows > max_stripe_height) {
                rows = max_stripe_height;
            }
            for (int c = 0; c < num_comps; ++c) {
                stripe_heights[c] = rows;
            }
            if (typesize == 1) {
                uint8_t *stripe_output = decoded8.data() +
                    static_cast<int64_t>(rows_done) * width * num_comps;
                needs_more = decompressor.pull_stripe(stripe_output, stripe_heights.data(),
                                                       nullptr, nullptr, nullptr,
                                                       precisions.data());
            } else if (typesize == 2) {
                // Kakadu's 16-bit pull_stripe overload is typed as
                // kdu_int16*, but with is_signed=false the buffer is
                // explicitly used as unsigned 16-bit storage.
                kdu_int16 *stripe_output = reinterpret_cast<kdu_int16*>(decoded16.data()) +
                    static_cast<int64_t>(rows_done) * width * num_comps;
                needs_more = decompressor.pull_stripe(stripe_output, stripe_heights.data(),
                                                       nullptr, nullptr, nullptr,
                                                       precisions.data(),
                                                       (const bool*)is_signed.get());
            }
            rows_done += rows;
            if (debug) {
                fprintf(stderr, "[blosc2_grok] Kakadu decoder: pulled rows=%d/%d needs_more=%d\n",
                        rows_done, height, needs_more ? 1 : 0);
            }
            if (!needs_more) {
                break;
            }
        }

        if (rows_done != height) {
            if (debug) {
                fprintf(stderr, "[blosc2_grok] Kakadu decoder error: incomplete stripe decode rows=%d/%d\n",
                        rows_done, height);
            }
            decompressor.reset();
            codestream.destroy();
            return -1;
        }
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu decoder: resetting stripe decompressor\n");
        }
        decompressor.reset();
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu decoder: stripe decompressor reset done\n");
        }
        if (typesize == 1) {
            std::memcpy(output, decoded8.data(), static_cast<size_t>(expected));
        } else if (typesize == 2) {
            std::memcpy(output, decoded16.data(), static_cast<size_t>(expected));
        }
        codestream.destroy();
        if (debug) {
            fprintf(stderr, "[blosc2_grok] Kakadu decoder: codestream destroyed\n");
        }
        if (is_jp2) {
            jp2.close();
            family.close();
        }
    } catch (kdu_exception) {
        return -1;
    }

    if (debug) {
        fprintf(stderr, "[blosc2_grok] Kakadu decoder: complete\n");
    }
    return output_len;
}

// J2K plugin capability entry point.
extern "C" int blosc2_kakadu_j2k_supports(const j2k_codec_request_t *request) {
    if (request == nullptr) {
        return 0;
    }
    return kakadu_supports_layout(request->precision_bits, request->num_components);
}

// J2K plugin encoder entry point.
extern "C" int blosc2_kakadu_j2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk,
    const j2k_codec_request_t * /*request*/
) {
    return kakadu_encode(input, input_len, output, output_len, meta, cparams, chunk, false);
}

// J2K plugin decoder entry point.
extern "C" int blosc2_kakadu_j2k_decoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_dparams *dparams,
    const void *chunk,
    const j2k_codec_request_t * /*request*/
) {
    return kakadu_decode(input, input_len, output, output_len, meta, dparams, chunk);
}

// HTJ2K plugin capability entry point.
extern "C" int blosc2_kakadu_htj2k_supports(const htj2k_codec_request_t *request) {
    if (request == nullptr) {
        return 0;
    }
    return kakadu_supports_layout(request->precision_bits, request->num_components);
}

// HTJ2K plugin encoder entry point.
extern "C" int blosc2_kakadu_htj2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* chunk,
    const htj2k_codec_request_t * /*request*/
) {
    return kakadu_encode(input, input_len, output, output_len, meta, cparams, chunk, true);
}

// HTJ2K plugin decoder entry point.
extern "C" int blosc2_kakadu_htj2k_decoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_dparams *dparams,
    const void *chunk,
    const htj2k_codec_request_t * /*request*/
) {
    return kakadu_decode(input, input_len, output, output_len, meta, dparams, chunk);
}
