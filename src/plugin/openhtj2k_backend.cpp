/*********************************************************************
 * blosc2_grok: OpenHTJ2K backend for HTJ2K replacement compression.
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
#include "b2nd.h"
#include "j2k_codec_api.h"

#include "encoder.hpp"
#include "decoder.hpp"

#ifdef BLOSC2_MAX_DIM
#define BLOSC2_GROK_MAX_DIM BLOSC2_MAX_DIM
#else
#define BLOSC2_GROK_MAX_DIM B2ND_MAX_DIM
#endif

namespace {

constexpr uint8_t OPENHTJ2K_NO_QFACTOR = 0xFF;

bool load_b2nd_info(blosc2_cparams *cparams,
                    int64_t &dim_x, int64_t &dim_y,
                    int32_t &num_comps, int32_t &typesize) {
    uint8_t *content = nullptr;
    int32_t content_len = 0;
    if (cparams == nullptr || cparams->schunk == nullptr ||
        blosc2_meta_get((blosc2_schunk*)cparams->schunk, "b2nd",
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
    if (rc < 0) {
        free(dtype);
        return false;
    }
    free(dtype);

    uint32_t igdim = 0;
    for (int i = 0; i < ndim; ++i) {
        if (blockshape[i] == 1) {
            igdim++;
        } else {
            break;
        }
    }
    if ((ndim - igdim) < 2) {
        return false;
    }

    dim_y = blockshape[igdim];
    dim_x = blockshape[igdim + 1];
    num_comps = 1;
    if ((ndim - igdim) == 3) {
        num_comps = blockshape[igdim + 2];
    }
    typesize = ((blosc2_schunk*)cparams->schunk)->typesize;
    return true;
}

uint8_t choose_dwt_levels(int64_t width, int64_t height) {
    uint8_t levels = 0;
    int64_t n = width < height ? width : height;
    while (n >= 2 && levels < 5) {
        n >>= 1;
        levels++;
    }
    return levels;
}

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

extern "C" int blosc2_openhtj2k_supports(const j2k_codec_request_t *request) {
    if (request == nullptr) {
        return 0;
    }
    if (!(request->codec_kind == J2K_CODEC_KIND_HTJ2K ||
          request->codec_kind == J2K_CODEC_KIND_UNKNOWN)) {
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

extern "C" int blosc2_openhtj2k_encoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t meta,
    blosc2_cparams* cparams,
    const void* /*chunk*/,
    const j2k_codec_request_t *request
) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    if (!blosc2_openhtj2k_supports(request) ||
        request->codec_kind != J2K_CODEC_KIND_HTJ2K) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] OpenHTJ2K encode supports only HTJ2K requests\n");
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

    const bool lossless = (request->flags & J2K_CODEC_REQUEST_FLAG_LOSSLESS) != 0 && meta == 0;
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
    qcd.base_step = lossless ? std::ldexp(1.0, -precision)
                             : std::ldexp(1.0, -(precision + 5));

    std::vector<uint8_t> encoded;
    try {
        open_htj2k::openhtj2k_encoder encoder("", plane_ptrs, siz, cod, qcd,
                                              OPENHTJ2K_NO_QFACTOR,
                                              false, 0, 0);
        encoder.set_output_buffer(encoded);
        size_t total_size = encoder.invoke();
        if (debug) {
            fprintf(stderr, "[blosc2_grok] OpenHTJ2K encoded bytes=%zu\n", total_size);
        }
        if (total_size > static_cast<size_t>(output_len)) {
            return 0;
        }
        std::memcpy(output, encoded.data(), total_size);
        return static_cast<int>(total_size);
    } catch (const std::exception &exc) {
        if (debug) {
            fprintf(stderr, "[blosc2_grok] OpenHTJ2K encoder exception: %s\n", exc.what());
        }
        return -1;
    }
}

extern "C" int blosc2_openhtj2k_decoder(
    const uint8_t *input,
    int32_t input_len,
    uint8_t *output,
    int32_t output_len,
    uint8_t /*meta*/,
    blosc2_dparams * /*dparams*/,
    const void * /*chunk*/,
    const j2k_codec_request_t * /*request*/
) {
    const bool debug = std::getenv("BLOSC2_GROK_DEBUG") != nullptr;
    try {
        open_htj2k::openhtj2k_decoder decoder(input, static_cast<size_t>(input_len), 0, 0);
        decoder.parse();
        const uint16_t num_comps = decoder.get_num_component();
        if (!(num_comps == 1 || num_comps == 3)) {
            return -1;
        }

        std::vector<uint32_t> width(num_comps);
        std::vector<uint32_t> height(num_comps);
        std::vector<uint8_t> depth(num_comps);
        std::vector<bool> is_signed(num_comps);
        std::vector<std::unique_ptr<int32_t[]>> planes;
        std::vector<int32_t*> plane_ptrs;
        planes.reserve(num_comps);
        plane_ptrs.reserve(num_comps);

        for (uint16_t c = 0; c < num_comps; ++c) {
            width[c] = decoder.get_component_width(c);
            height[c] = decoder.get_component_height(c);
            depth[c] = decoder.get_component_depth(c);
            is_signed[c] = decoder.get_component_signedness(c);
            if (c > 0 && (width[c] != width[0] || height[c] != height[0])) {
                return -1;
            }
            if (is_signed[c] || depth[c] > 16) {
                return -1;
            }
            planes.emplace_back(new int32_t[static_cast<size_t>(width[c]) * height[c]]);
            plane_ptrs.push_back(planes.back().get());
        }

        decoder.invoke(plane_ptrs, width, height, depth, is_signed);

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
            fprintf(stderr, "[blosc2_grok] OpenHTJ2K decoder exception: %s\n", exc.what());
        }
        return -1;
    }
}
