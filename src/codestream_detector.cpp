/*********************************************************************
 * blosc2_htj2k: JPEG2000 codestream-family detection helpers.
 *
 * This file owns only lightweight codestream inspection.  It does not know
 * about Blosc2, Grok state or runtime replacement plugins.
 *
 * Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
 * https://blosc.org
 * License: GNU Affero General Public License v3.0 (see LICENSE.txt)
 *********************************************************************/

#include "codestream_detector.h"

#include <cstddef>
#include <cstring>

#include "grok.h"

namespace blosc2_htj2k_detail {
namespace {

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

}  // namespace

CodecFamily detect_codestream_family(const uint8_t *data, int32_t data_len) {
    if (data == nullptr || data_len <= 0) {
        return CodecFamily::UNKNOWN;
    }
    if (has_jp2_signature(data, data_len)) {
        return detect_jp2_container_codestream_family(data, data_len);
    }
    return detect_raw_codestream_family(data, data_len);
}

}  // namespace blosc2_htj2k_detail
