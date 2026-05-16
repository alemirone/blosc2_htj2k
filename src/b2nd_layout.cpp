/*********************************************************************
 * blosc2_htj2k: local B2ND metalayer reader.
 *
 * The B2ND metalayer is a small MsgPack array:
 *   [version, ndim, shape, chunkshape, blockshape, dtype_format, dtype]
 * Parsing this locally avoids a runtime dependency on b2nd_deserialize_meta().
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "b2nd_layout.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace blosc2_htj2k_detail {
namespace {

class MsgpackReader {
  public:
    MsgpackReader(const uint8_t *data, int32_t len)
        : cur(data), end(data + (len > 0 ? len : 0)) {}

    bool read_array_size(uint32_t &size) {
        uint8_t b = 0;
        if (!read_u8(b)) {
            return false;
        }
        if ((b & 0xf0) == 0x90) {
            size = b & 0x0f;
            return true;
        }
        if (b == 0xdc) {
            uint16_t v = 0;
            if (!read_be16(v)) {
                return false;
            }
            size = v;
            return true;
        }
        if (b == 0xdd) {
            return read_be32(size);
        }
        return false;
    }

    bool read_integer(int64_t &value) {
        uint8_t b = 0;
        if (!read_u8(b)) {
            return false;
        }
        if (b <= 0x7f) {
            value = b;
            return true;
        }
        if (b >= 0xe0) {
            value = static_cast<int8_t>(b);
            return true;
        }
        switch (b) {
            case 0xcc: {
                uint8_t v = 0;
                if (!read_u8(v)) {
                    return false;
                }
                value = v;
                return true;
            }
            case 0xcd: {
                uint16_t v = 0;
                if (!read_be16(v)) {
                    return false;
                }
                value = v;
                return true;
            }
            case 0xce: {
                uint32_t v = 0;
                if (!read_be32(v)) {
                    return false;
                }
                value = v;
                return true;
            }
            case 0xcf: {
                uint64_t v = 0;
                if (!read_be64(v) ||
                    v > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
                    return false;
                }
                value = static_cast<int64_t>(v);
                return true;
            }
            case 0xd0: {
                uint8_t v = 0;
                if (!read_u8(v)) {
                    return false;
                }
                value = static_cast<int8_t>(v);
                return true;
            }
            case 0xd1: {
                uint16_t v = 0;
                if (!read_be16(v)) {
                    return false;
                }
                value = static_cast<int16_t>(v);
                return true;
            }
            case 0xd2: {
                uint32_t v = 0;
                if (!read_be32(v)) {
                    return false;
                }
                value = static_cast<int32_t>(v);
                return true;
            }
            case 0xd3: {
                uint64_t v = 0;
                if (!read_be64(v)) {
                    return false;
                }
                value = static_cast<int64_t>(v);
                return true;
            }
            default:
                return false;
        }
    }

    bool read_string(std::string &value) {
        uint8_t b = 0;
        if (!read_u8(b)) {
            return false;
        }

        uint32_t size = 0;
        if ((b & 0xe0) == 0xa0) {
            size = b & 0x1f;
        } else if (b == 0xd9) {
            uint8_t v = 0;
            if (!read_u8(v)) {
                return false;
            }
            size = v;
        } else if (b == 0xda) {
            uint16_t v = 0;
            if (!read_be16(v)) {
                return false;
            }
            size = v;
        } else if (b == 0xdb) {
            if (!read_be32(size)) {
                return false;
            }
        } else {
            return false;
        }

        if (static_cast<size_t>(end - cur) < size) {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(cur), size);
        cur += size;
        return true;
    }

  private:
    bool read_u8(uint8_t &value) {
        if (cur >= end) {
            return false;
        }
        value = *cur++;
        return true;
    }

    bool read_be16(uint16_t &value) {
        if (static_cast<size_t>(end - cur) < 2) {
            return false;
        }
        value = static_cast<uint16_t>((cur[0] << 8) | cur[1]);
        cur += 2;
        return true;
    }

    bool read_be32(uint32_t &value) {
        if (static_cast<size_t>(end - cur) < 4) {
            return false;
        }
        value = (static_cast<uint32_t>(cur[0]) << 24) |
                (static_cast<uint32_t>(cur[1]) << 16) |
                (static_cast<uint32_t>(cur[2]) << 8) |
                static_cast<uint32_t>(cur[3]);
        cur += 4;
        return true;
    }

    bool read_be64(uint64_t &value) {
        if (static_cast<size_t>(end - cur) < 8) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | cur[i];
        }
        cur += 8;
        return true;
    }

    const uint8_t *cur = nullptr;
    const uint8_t *end = nullptr;
};

bool read_int32_array(MsgpackReader &reader,
                      int8_t expected_size,
                      std::array<int32_t, BLOSC2_HTJ2K_LAYOUT_MAX_DIM> &values) {
    uint32_t size = 0;
    if (!reader.read_array_size(size) || size != static_cast<uint32_t>(expected_size)) {
        return false;
    }
    for (uint32_t i = 0; i < size; ++i) {
        int64_t v = 0;
        if (!reader.read_integer(v) ||
            v < (std::numeric_limits<int32_t>::min)() ||
            v > (std::numeric_limits<int32_t>::max)()) {
            return false;
        }
        values[i] = static_cast<int32_t>(v);
    }
    return true;
}

bool read_int64_array(MsgpackReader &reader,
                      int8_t expected_size,
                      std::array<int64_t, BLOSC2_HTJ2K_LAYOUT_MAX_DIM> &values) {
    uint32_t size = 0;
    if (!reader.read_array_size(size) || size != static_cast<uint32_t>(expected_size)) {
        return false;
    }
    for (uint32_t i = 0; i < size; ++i) {
        if (!reader.read_integer(values[i])) {
            return false;
        }
    }
    return true;
}

bool parse_b2nd_meta(const uint8_t *content, int32_t content_len, B2ndLayout &layout) {
    MsgpackReader reader(content, content_len);

    uint32_t top_size = 0;
    if (!reader.read_array_size(top_size) || top_size != 7) {
        return false;
    }

    int64_t version = 0;
    int64_t ndim = 0;
    int64_t dtype_format = 0;
    if (!reader.read_integer(version) ||
        !reader.read_integer(ndim) ||
        ndim <= 0 ||
        ndim > BLOSC2_HTJ2K_LAYOUT_MAX_DIM) {
        return false;
    }

    layout.ndim = static_cast<int8_t>(ndim);
    if (!read_int64_array(reader, layout.ndim, layout.shape) ||
        !read_int32_array(reader, layout.ndim, layout.chunkshape) ||
        !read_int32_array(reader, layout.ndim, layout.blockshape) ||
        !reader.read_integer(dtype_format) ||
        dtype_format < (std::numeric_limits<int8_t>::min)() ||
        dtype_format > (std::numeric_limits<int8_t>::max)() ||
        !reader.read_string(layout.dtype)) {
        return false;
    }
    layout.dtype_format = static_cast<int8_t>(dtype_format);
    (void)version;
    return true;
}

}  // namespace

bool read_b2nd_layout(blosc2_cparams *cparams, B2ndLayout &layout) {
    if (cparams == nullptr || cparams->schunk == nullptr) {
        return false;
    }

    uint8_t *content = nullptr;
    int32_t content_len = 0;
    if (blosc2_meta_get((blosc2_schunk *)cparams->schunk, "b2nd",
                        &content, &content_len) < 0) {
        return false;
    }

    bool ok = parse_b2nd_meta(content, content_len, layout);
    free(content);
    if (!ok) {
        return false;
    }
    layout.typesize = static_cast<int32_t>(((blosc2_schunk *)cparams->schunk)->typesize);
    return layout.typesize > 0;
}

bool image_layout_from_b2nd(const B2ndLayout &layout,
                            int64_t &dim_x,
                            int64_t &dim_y,
                            int32_t &num_comps) {
    uint32_t igdim = 0;
    for (int i = 0; i < layout.ndim; ++i) {
        if (layout.blockshape[i] == 1) {
            igdim++;
        } else {
            break;
        }
    }
    if ((layout.ndim - static_cast<int8_t>(igdim)) < 2) {
        return false;
    }

    dim_y = layout.blockshape[igdim];
    dim_x = layout.blockshape[igdim + 1];
    num_comps = 1;
    if ((layout.ndim - static_cast<int8_t>(igdim)) == 3) {
        num_comps = layout.blockshape[igdim + 2];
    }
    return dim_x > 0 && dim_y > 0 && num_comps > 0;
}

}  // namespace blosc2_htj2k_detail
