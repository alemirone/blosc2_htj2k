/*********************************************************************
 * blosc2_htj2k: float32 quantization frame helpers.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "float_quantization.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace blosc2_htj2k_detail {
namespace {

constexpr uint8_t FLOAT_FRAME_MAGIC[8] = {'B', '2', 'J', 'P', 'F', '3', '2', '\0'};
constexpr uint16_t FLOAT_FRAME_VERSION = 1;
constexpr int32_t FLOAT_FRAME_HEADER_SIZE = 64;

uint64_t qmax_for_bits(uint32_t bits) {
    if (bits == 8) {
        return 255u;
    }
    if (bits == 16) {
        return 65535u;
    }
    return 4294967295ull;
}

void write_u16(uint8_t *dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xffu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

void write_u64(uint8_t *dst, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffu);
    }
}

uint16_t read_u16(const uint8_t *src) {
    return static_cast<uint16_t>(src[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(src[1]) << 8);
}

uint64_t read_u64(const uint8_t *src) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(src[i]) << (8 * i);
    }
    return value;
}

void write_f64(uint8_t *dst, double value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64 bit");
    std::memcpy(&bits, &value, sizeof(bits));
    write_u64(dst, bits);
}

double read_f64(const uint8_t *src) {
    uint64_t bits = read_u64(src);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_quantized_value(uint8_t *dst, uint32_t bits, uint64_t value) {
    if (bits == 8) {
        dst[0] = static_cast<uint8_t>(value);
    } else if (bits == 16) {
        uint16_t v = static_cast<uint16_t>(value);
        std::memcpy(dst, &v, sizeof(v));
    } else {
        uint32_t v = static_cast<uint32_t>(value);
        std::memcpy(dst, &v, sizeof(v));
    }
}

uint64_t read_quantized_value(const uint8_t *src, uint32_t bits) {
    if (bits == 8) {
        return src[0];
    }
    if (bits == 16) {
        uint16_t v = 0;
        std::memcpy(&v, src, sizeof(v));
        return v;
    }
    uint32_t v = 0;
    std::memcpy(&v, src, sizeof(v));
    return v;
}

}  // namespace

bool is_float32_dtype(const B2ndLayout &layout) {
    return layout.typesize == 4 &&
           (layout.dtype == "<f4" || layout.dtype == "=f4" ||
            layout.dtype == "|f4" || layout.dtype == "float32");
}

bool is_float_dtype(const B2ndLayout &layout) {
    return layout.dtype.find("f4") != std::string::npos ||
           layout.dtype.find("f8") != std::string::npos ||
           layout.dtype == "float32" ||
           layout.dtype == "float64";
}

int32_t float_frame_header_size() {
    return FLOAT_FRAME_HEADER_SIZE;
}

bool has_float_frame(const uint8_t *input, int32_t input_len) {
    return input != nullptr &&
           input_len >= FLOAT_FRAME_HEADER_SIZE &&
           std::memcmp(input, FLOAT_FRAME_MAGIC, sizeof(FLOAT_FRAME_MAGIC)) == 0;
}

bool quantize_float32_chunk(const uint8_t *input,
                            int32_t input_len,
                            const B2ndLayout &layout,
                            const FloatRuntimeConfig &config,
                            bool inner_lossy,
                            QuantizedFloatChunk &chunk,
                            std::string &error) {
    if (!config.valid) {
        error = config.error;
        return false;
    }
    if (!config.enabled) {
        error = "float32 input requires opt-in float mode";
        return false;
    }
    if (!is_float32_dtype(layout)) {
        error = "float mode v1 only supports little-endian/native float32 chunks";
        return false;
    }
    if (input == nullptr || input_len <= 0 || (input_len % 4) != 0) {
        error = "invalid float32 input length";
        return false;
    }
    const int64_t n = input_len / 4;
    const auto *values = reinterpret_cast<const float *>(input);
    double raw_min = std::numeric_limits<double>::infinity();
    double raw_max = -std::numeric_limits<double>::infinity();
    for (int64_t i = 0; i < n; ++i) {
        double v = static_cast<double>(values[i]);
        if (!std::isfinite(v)) {
            error = "float mode v1 rejects NaN and Inf values";
            return false;
        }
        raw_min = std::min(raw_min, v);
        raw_max = std::max(raw_max, v);
    }

    double scale_min = config.clamp_min_set ? config.clamp_min : raw_min;
    double scale_max = config.clamp_max_set ? config.clamp_max : raw_max;
    if (!std::isfinite(scale_min) || !std::isfinite(scale_max) || scale_min > scale_max) {
        error = "invalid float mode scale range";
        return false;
    }

    chunk.frame = {};
    chunk.frame.quant_bits = config.quant_bits;
    chunk.frame.nan_policy = config.nan_policy;
    chunk.frame.flags = 0;
    if (config.clamp_min_set) {
        chunk.frame.flags |= FLOAT_FRAME_FLAG_CLAMP_MIN;
    }
    if (config.clamp_max_set) {
        chunk.frame.flags |= FLOAT_FRAME_FLAG_CLAMP_MAX;
    }
    if (inner_lossy) {
        chunk.frame.flags |= FLOAT_FRAME_FLAG_INNER_LOSSY;
    }
    chunk.frame.scale_min = scale_min;
    chunk.frame.scale_max = scale_max;
    chunk.frame.raw_min = raw_min;
    chunk.frame.raw_max = raw_max;

    if (scale_min == scale_max) {
        chunk.frame.flags |= FLOAT_FRAME_FLAG_CONSTANT;
        chunk.bytes.clear();
        return true;
    }

    const uint32_t qbytes = config.quant_bits / 8;
    const uint64_t qmax = qmax_for_bits(config.quant_bits);
    chunk.bytes.resize(static_cast<size_t>(n) * qbytes);
    const double denom = scale_max - scale_min;
    for (int64_t i = 0; i < n; ++i) {
        double clipped = std::min(std::max(static_cast<double>(values[i]), scale_min), scale_max);
        double normalized = (clipped - scale_min) / denom;
        uint64_t q = static_cast<uint64_t>(std::llround(normalized * static_cast<double>(qmax)));
        if (q > qmax) {
            q = qmax;
        }
        write_quantized_value(chunk.bytes.data() + static_cast<size_t>(i) * qbytes,
                              config.quant_bits, q);
    }
    return true;
}

bool write_float_frame_header(const FloatFrame &frame,
                              uint8_t *output,
                              int32_t output_len,
                              std::string &error) {
    if (output == nullptr || output_len < FLOAT_FRAME_HEADER_SIZE) {
        error = "output buffer too small for float frame header";
        return false;
    }
    std::memset(output, 0, FLOAT_FRAME_HEADER_SIZE);
    std::memcpy(output, FLOAT_FRAME_MAGIC, sizeof(FLOAT_FRAME_MAGIC));
    write_u16(output + 8, FLOAT_FRAME_VERSION);
    write_u16(output + 10, static_cast<uint16_t>(FLOAT_FRAME_HEADER_SIZE));
    output[12] = static_cast<uint8_t>(frame.quant_bits);
    output[13] = static_cast<uint8_t>(frame.nan_policy);
    write_u16(output + 14, frame.flags);
    write_u64(output + 16, frame.payload_nbytes);
    write_f64(output + 24, frame.scale_min);
    write_f64(output + 32, frame.scale_max);
    write_f64(output + 40, frame.raw_min);
    write_f64(output + 48, frame.raw_max);
    return true;
}

bool read_float_frame_header(const uint8_t *input,
                             int32_t input_len,
                             FloatFrame &frame,
                             const uint8_t *&payload,
                             int32_t &payload_len,
                             std::string &error) {
    if (!has_float_frame(input, input_len)) {
        error = "missing float frame header";
        return false;
    }
    uint16_t version = read_u16(input + 8);
    uint16_t header_size = read_u16(input + 10);
    if (version != FLOAT_FRAME_VERSION || header_size != FLOAT_FRAME_HEADER_SIZE) {
        error = "unsupported float frame version or header size";
        return false;
    }
    frame = {};
    frame.quant_bits = input[12];
    frame.nan_policy = input[13];
    frame.flags = read_u16(input + 14);
    frame.payload_nbytes = read_u64(input + 16);
    frame.scale_min = read_f64(input + 24);
    frame.scale_max = read_f64(input + 32);
    frame.raw_min = read_f64(input + 40);
    frame.raw_max = read_f64(input + 48);
    if (!(frame.quant_bits == 8 || frame.quant_bits == 16 || frame.quant_bits == 32)) {
        error = "unsupported float frame quantization precision";
        return false;
    }
    if (frame.nan_policy != 0) {
        error = "unsupported float frame NaN policy";
        return false;
    }
    if (frame.payload_nbytes > static_cast<uint64_t>(input_len - FLOAT_FRAME_HEADER_SIZE)) {
        error = "float frame payload is truncated";
        return false;
    }
    payload = input + FLOAT_FRAME_HEADER_SIZE;
    payload_len = static_cast<int32_t>(frame.payload_nbytes);
    return true;
}

bool dequantize_float32_chunk(const FloatFrame &frame,
                              const uint8_t *quantized,
                              int32_t quantized_len,
                              uint8_t *output,
                              int32_t output_len,
                              std::string &error) {
    if (output == nullptr || output_len < 0 || (output_len % 4) != 0) {
        error = "invalid float32 output length";
        return false;
    }
    const int64_t n = output_len / 4;
    auto *values = reinterpret_cast<float *>(output);
    if ((frame.flags & FLOAT_FRAME_FLAG_CONSTANT) != 0) {
        float constant = static_cast<float>(frame.scale_min);
        for (int64_t i = 0; i < n; ++i) {
            values[i] = constant;
        }
        return true;
    }

    const uint32_t qbytes = frame.quant_bits / 8;
    const int64_t expected = n * static_cast<int64_t>(qbytes);
    if (quantized == nullptr || quantized_len < expected) {
        error = "decoded integer payload is too small for float dequantization";
        return false;
    }
    const uint64_t qmax = qmax_for_bits(frame.quant_bits);
    const double scale = (frame.scale_max - frame.scale_min) / static_cast<double>(qmax);
    for (int64_t i = 0; i < n; ++i) {
        uint64_t q = read_quantized_value(quantized + static_cast<size_t>(i) * qbytes,
                                          frame.quant_bits);
        values[i] = static_cast<float>(frame.scale_min + static_cast<double>(q) * scale);
    }
    return true;
}

}  // namespace blosc2_htj2k_detail
