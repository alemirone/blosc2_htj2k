/*********************************************************************
 * blosc2_htj2k: OpenHTJ2K backend for HTJ2K replacement compression.
 *
 * This backend uses OpenHTJ2K directly through its C++ library API.  It is
 * compiled only when OpenHTJ2K headers and libraries are available.
 *
 * Copyright (c) 2026  Alessandro Mirone
 * License: GNU Affero General Public License v3.0
 **********************************************************************/

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

#include "blosc2.h"
#include "b2nd_layout.h"
#include "htj2k_codec_api.h"

#include "encoder.hpp"
#include "decoder.hpp"

// Function responsibility map:
//
// Capability boundary:
// - blosc2_openhtj2k_supports(): declare exactly which HTJ2K requests this backend can handle.
//
// Blosc2 layout adaptation:
// - load_b2nd_info(): read dimensions, component count and sample size from b2nd metadata.
// - fill_planar_input(): convert Blosc2 interleaved chunk bytes to OpenHTJ2K planar buffers.
//
// OpenHTJ2K codec policy:
// - choose_dwt_levels(): choose a conservative decomposition count for small chunk images.
// - blosc2_openhtj2k_encoder(): encode HTJ2K uint8/uint16 gray/RGB chunks.
// - blosc2_openhtj2k_decoder(): decode codestreams back to Blosc2 interleaved bytes.
namespace {
using blosc2_htj2k_detail::B2ndLayout;
using blosc2_htj2k_detail::image_layout_from_b2nd;
using blosc2_htj2k_detail::read_b2nd_layout;

constexpr uint8_t OPENHTJ2K_NO_QFACTOR = 0xFF;

// Read transparent Blosc2 b2nd layout and map (..., Y, X[, C]) to codec
// coordinates X/Y plus component count.
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

// Pick a bounded wavelet decomposition count; small chunks cannot support many
// levels, and five levels is enough for this backend's test/optional role.
uint8_t choose_dwt_levels(int64_t width, int64_t height) {
    uint8_t levels = 0;
    int64_t n = width < height ? width : height;
    while (n >= 2 && levels < 5) {
        n >>= 1;
        levels++;
    }
    return levels;
}

// Convert Blosc2's interleaved uint8/uint16 memory layout to the planar int32
// buffers expected by OpenHTJ2K.
bool fill_planar_input(const uint8_t *input,
                       int64_t width,
                       int64_t height,
                       int32_t num_comps,
                       int32_t typesize,
                       std::vector<std::unique_ptr<int32_t[]>> &planes,
                       std::vector<int32_t*> &plane_ptrs) {
    planes.clear();
    plane_ptrs.clear();
    planes.reserve(static_cast<size_t>(num_comps));
    plane_ptrs.reserve(static_cast<size_t>(num_comps));

    const int64_t pixels = width * height;
    for (int32_t c = 0; c < num_comps; ++c) {
        planes.emplace_back(new int32_t[static_cast<size_t>(pixels)]);
        plane_ptrs.push_back(planes.back().get());
    }

    int64_t src_index = 0;
    for (int64_t p = 0; p < pixels; ++p) {
        for (int32_t c = 0; c < num_comps; ++c) {
            if (typesize == 1) {
                plane_ptrs[c][p] = input[src_index];
            } else if (typesize == 2) {
                uint16_t value = 0;
                std::memcpy(&value, input + src_index, sizeof(value));
                plane_ptrs[c][p] = value;
            } else {
                return false;
            }
            src_index += typesize;
        }
    }
    return true;
}

}  // namespace

// Report whether the backend can satisfy the current HTJ2K request.
extern "C" int blosc2_openhtj2k_supports(const htj2k_codec_request_t *request) {
    if (request == nullptr) {
        return 0;
    }
    if (request->precision_bits != 0 &&
        !(request->precision_bits == 8 || request->precision_bits == 16)) {
        return 0;
    }
    if (request->num_components != 0 &&
        !(request->num_components == 1 || request->num_components == 3)) {
        return 0;
    }
    return 1;
}

// Encode one Blosc2 chunk as an HTJ2K codestream using OpenHTJ2K directly.
extern "C" int blosc2_openhtj2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* /*chunk*/,
    const htj2k_codec_request_t *request
) {
    const bool debug = std::getenv("BLOSC2_HTJ2K_DEBUG") != nullptr;
    if (!blosc2_openhtj2k_supports(request)) {
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] OpenHTJ2K encode does not support this HTJ2K request\n");
        }
        return -1;
    }

    int64_t dim_x = 0;
    int64_t dim_y = 0;
    int32_t num_comps = 0;
    int32_t typesize = 0;
    if (!load_b2nd_info(cparams, dim_x, dim_y, num_comps, typesize)) {
        return -1;
    }
    if (request != nullptr && request->precision_bits != 0) {
        if (!(request->precision_bits == 8 || request->precision_bits == 16)) {
            return -1;
        }
        typesize = static_cast<int32_t>(request->precision_bits / 8);
    }
    if (!((num_comps == 1) || (num_comps == 3)) || !(typesize == 1 || typesize == 2)) {
        return -1;
    }
    const int32_t precision = typesize * 8;
    const int64_t expected_len = dim_x * dim_y * num_comps * typesize;
    if (input_len < expected_len) {
        return -1;
    }

    std::vector<std::unique_ptr<int32_t[]>> planes;
    std::vector<int32_t*> plane_ptrs;
    if (!fill_planar_input(input, dim_x, dim_y, num_comps, typesize, planes, plane_ptrs)) {
        return -1;
    }

    open_htj2k::siz_params siz{};
    siz.Rsiz = 0;
    siz.Xsiz = static_cast<uint32_t>(dim_x);
    siz.Ysiz = static_cast<uint32_t>(dim_y);
    siz.XOsiz = 0;
    siz.YOsiz = 0;
    siz.XTsiz = static_cast<uint32_t>(dim_x);
    siz.YTsiz = static_cast<uint32_t>(dim_y);
    siz.XTOsiz = 0;
    siz.YTOsiz = 0;
    siz.Csiz = static_cast<uint16_t>(num_comps);
    for (int32_t c = 0; c < num_comps; ++c) {
        siz.Ssiz.push_back(static_cast<uint8_t>(precision - 1));
        siz.XRsiz.push_back(1);
        siz.YRsiz.push_back(1);
    }

    const bool lossless = (request->flags & HTJ2K_CODEC_REQUEST_FLAG_LOSSLESS) != 0 && meta == 0;
    open_htj2k::cod_params cod{};
    cod.blkwidth = 4;       // OpenHTJ2K stores log2(block_size) - 2; 4 means 64.
    cod.blkheight = 4;
    cod.is_max_precincts = true;
    cod.use_SOP = false;
    cod.use_EPH = false;
    cod.progression_order = 0;  // LRCP
    cod.number_of_layers = 1;
    cod.use_color_trafo = (num_comps == 3) ? 1 : 0;
    cod.dwt_levels = choose_dwt_levels(dim_x, dim_y);
    cod.codeblock_style = 0x040;  // HT block coding.
    cod.transformation = lossless ? 1 : 0;

    open_htj2k::qcd_params qcd{};
    qcd.number_of_guardbits = 1;
    qcd.is_derived = false;
    // blosc2_htj2k uses codec_meta / 10.0 as a target compression ratio.  The
    // OpenHTJ2K library API exposes quantization controls rather than a byte
    // budget, so this keeps the same monotonic meaning with a conservative
    // quantization step.
    const double target_ratio = meta == 0 ? 1.0 : static_cast<double>(meta) / 10.0;
    qcd.base_step = lossless ? std::ldexp(1.0, -precision)
                             : std::ldexp(1.0, -(precision > 4 ? precision - 4 : precision)) *
                                   target_ratio;

    std::vector<uint8_t> encoded;
    try {
        open_htj2k::openhtj2k_encoder encoder("", plane_ptrs, siz, cod, qcd,
                                              OPENHTJ2K_NO_QFACTOR,
                                              false, 0, 0);
        encoder.set_output_buffer(encoded);
        size_t total_size = encoder.invoke();
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] OpenHTJ2K encoded bytes=%zu\n", total_size);
        }
        if (total_size > static_cast<size_t>(output_len)) {
            return 0;
        }
        std::memcpy(output, encoded.data(), total_size);
        return static_cast<int>(total_size);
    } catch (const std::exception &exc) {
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] OpenHTJ2K encoder exception: %s\n", exc.what());
        }
        return -1;
    }
}

// Decode one OpenHTJ2K codestream into the Blosc2 interleaved output buffer.
extern "C" int blosc2_openhtj2k_decoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t /*meta*/,
    blosc2_dparams * /*dparams*/,
    const void * /*chunk*/,
    const htj2k_codec_request_t * /*request*/
) {
    const bool debug = std::getenv("BLOSC2_HTJ2K_DEBUG") != nullptr;
    try {
        open_htj2k::openhtj2k_decoder decoder(input, static_cast<size_t>(input_len), 0, 0);
        decoder.parse();
        const uint16_t num_comps = decoder.get_num_component();
        if (!(num_comps == 1 || num_comps == 3)) {
            return -1;
        }

        std::vector<int32_t*> plane_ptrs;
        std::vector<uint32_t> width;
        std::vector<uint32_t> height;
        std::vector<uint8_t> depth;
        std::vector<bool> is_signed;
        struct PlaneCleanup {
            std::vector<int32_t*> &planes;
            ~PlaneCleanup() {
                for (int32_t *plane : planes) {
                    delete[] plane;
                }
            }
        } cleanup{plane_ptrs};
        decoder.invoke(plane_ptrs, width, height, depth, is_signed);
        if (plane_ptrs.size() != num_comps || width.size() != num_comps ||
            height.size() != num_comps || depth.size() != num_comps ||
            is_signed.size() != num_comps) {
            return -1;
        }

        for (uint16_t c = 0; c < num_comps; ++c) {
            if (c > 0 && (width[c] != width[0] || height[c] != height[0])) {
                return -1;
            }
            if (is_signed[c] || depth[c] > 16) {
                return -1;
            }
        }

        uint8_t max_depth = 0;
        for (uint8_t d : depth) {
            if (d > max_depth) {
                max_depth = d;
            }
        }
        const int32_t typesize = (max_depth <= 8) ? 1 : 2;
        const int64_t expected_len = static_cast<int64_t>(width[0]) *
                                     static_cast<int64_t>(height[0]) *
                                     static_cast<int64_t>(num_comps) *
                                     typesize;
        if (expected_len > output_len) {
            return -1;
        }

        int64_t dst_index = 0;
        const int64_t pixels = static_cast<int64_t>(width[0]) * height[0];
        for (int64_t p = 0; p < pixels; ++p) {
            for (uint16_t c = 0; c < num_comps; ++c) {
                int32_t sample = plane_ptrs[c][p];
                if (typesize == 1) {
                    output[dst_index++] = static_cast<uint8_t>(sample);
                } else {
                    uint16_t value = static_cast<uint16_t>(sample);
                    std::memcpy(output + dst_index, &value, sizeof(value));
                    dst_index += sizeof(value);
                }
            }
        }
        return static_cast<int>(expected_len);
    } catch (const std::exception &exc) {
        if (debug) {
            fprintf(stderr, "[blosc2_htj2k] OpenHTJ2K decoder exception: %s\n", exc.what());
        }
        return -1;
    }
}
