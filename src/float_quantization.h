/*********************************************************************
 * blosc2_htj2k: float32 quantization frame helpers.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#ifndef BLOSC2_HTJ2K_FLOAT_QUANTIZATION_H
#define BLOSC2_HTJ2K_FLOAT_QUANTIZATION_H

#include <cstdint>
#include <string>
#include <vector>

#include "b2nd_layout.h"
#include "runtime_config.h"

namespace blosc2_htj2k_detail {

constexpr uint16_t FLOAT_FRAME_FLAG_CLAMP_MIN = 0x01u;
constexpr uint16_t FLOAT_FRAME_FLAG_CLAMP_MAX = 0x02u;
constexpr uint16_t FLOAT_FRAME_FLAG_CONSTANT = 0x04u;
constexpr uint16_t FLOAT_FRAME_FLAG_INNER_LOSSY = 0x08u;

struct FloatFrame {
    uint32_t quant_bits = 0;
    uint32_t nan_policy = 0;
    uint16_t flags = 0;
    uint64_t payload_nbytes = 0;
    double scale_min = 0.0;
    double scale_max = 0.0;
    double raw_min = 0.0;
    double raw_max = 0.0;
};

struct QuantizedFloatChunk {
    FloatFrame frame;
    std::vector<uint8_t> bytes;
};

bool is_float32_dtype(const B2ndLayout &layout);
bool is_float_dtype(const B2ndLayout &layout);

int32_t float_frame_header_size();
bool has_float_frame(const uint8_t *input, int32_t input_len);

bool quantize_float32_chunk(const uint8_t *input,
                            int32_t input_len,
                            const B2ndLayout &layout,
                            const FloatRuntimeConfig &config,
                            bool inner_lossy,
                            QuantizedFloatChunk &chunk,
                            std::string &error);

bool write_float_frame_header(const FloatFrame &frame,
                              uint8_t *output,
                              int32_t output_len,
                              std::string &error);

bool read_float_frame_header(const uint8_t *input,
                             int32_t input_len,
                             FloatFrame &frame,
                             const uint8_t *&payload,
                             int32_t &payload_len,
                             std::string &error);

bool dequantize_float32_chunk(const FloatFrame &frame,
                              const uint8_t *quantized,
                              int32_t quantized_len,
                              uint8_t *output,
                              int32_t output_len,
                              std::string &error);

}  // namespace blosc2_htj2k_detail

#endif  // BLOSC2_HTJ2K_FLOAT_QUANTIZATION_H
