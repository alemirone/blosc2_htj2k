#include <b2nd.h>
#include <blosc2.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef BLOSC_CODEC_HTJ2K
#define BLOSC_CODEC_HTJ2K 40
#endif

namespace {

void fail(const std::string &message) {
    std::cerr << "ERROR: " << message << "\n";
    std::exit(1);
}

void check(int rc, const std::string &what) {
    if (rc < 0) {
        fail(what + " failed with code " + std::to_string(rc));
    }
}

std::vector<uint16_t> make_stack() {
    constexpr int64_t frames = 4;
    constexpr int64_t height = 64;
    constexpr int64_t width = 96;
    std::vector<uint16_t> data(frames * height * width);
    for (int64_t z = 0; z < frames; ++z) {
        for (int64_t y = 0; y < height; ++y) {
            for (int64_t x = 0; x < width; ++x) {
                const auto value = static_cast<uint32_t>(
                    22000 + ((x * 37 + y * 113 + z * 251) % 17000));
                data[(z * height + y) * width + x] = static_cast<uint16_t>(value);
            }
        }
    }
    return data;
}

int parse_codec_meta(int argc, char **argv) {
    int codec_meta = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--codec-meta" && i + 1 < argc) {
            codec_meta = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--codec-meta N]\n"
                      << "  codec_meta=0 is lossless; non-zero means target cratio=N/10.\n";
            std::exit(0);
        } else {
            fail("unknown argument: " + arg);
        }
    }
    if (codec_meta < 0 || codec_meta > 255) {
        fail("--codec-meta must be in [0, 255]");
    }
    return codec_meta;
}

}  // namespace

int main(int argc, char **argv) {
    const int codec_meta = parse_codec_meta(argc, argv);

    blosc2_init();

    const int codec_id = blosc2_compname_to_compcode("htj2k");
    if (codec_id != BLOSC_CODEC_HTJ2K) {
        fail("updated c-blosc2 with registered codec name 'htj2k' was not found");
    }

    const int8_t ndim = 3;
    int64_t shape[] = {4, 64, 96};
    int32_t chunkshape[] = {1, 64, 96};
    int32_t blockshape[] = {1, 64, 96};
    const int32_t typesize = static_cast<int32_t>(sizeof(uint16_t));

    auto input = make_stack();
    const int64_t nbytes = static_cast<int64_t>(input.size() * sizeof(uint16_t));

    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.compcode = static_cast<uint8_t>(codec_id);
    cparams.compcode_meta = static_cast<uint8_t>(codec_meta);
    cparams.clevel = 5;
    cparams.typesize = typesize;
    cparams.splitmode = BLOSC_NEVER_SPLIT;
    std::fill(std::begin(cparams.filters), std::end(cparams.filters), BLOSC_NOFILTER);
    std::fill(std::begin(cparams.filters_meta), std::end(cparams.filters_meta), 0);

    blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
    blosc2_storage storage = BLOSC2_STORAGE_DEFAULTS;
    storage.cparams = &cparams;
    storage.dparams = &dparams;

    b2nd_context_t *ctx = b2nd_create_ctx(
        &storage, ndim, shape, chunkshape, blockshape, nullptr, 0, nullptr, 0);
    if (ctx == nullptr) {
        fail("b2nd_create_ctx failed");
    }

    b2nd_array_t *array = nullptr;
    check(b2nd_from_cbuffer(ctx, &array, input.data(), nbytes), "b2nd_from_cbuffer");

    std::vector<uint16_t> decoded(input.size());
    check(b2nd_to_cbuffer(array, decoded.data(), nbytes), "b2nd_to_cbuffer");

    uint32_t max_abs_error = 0;
    uint64_t sum_abs_error = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        const uint32_t a = input[i];
        const uint32_t b = decoded[i];
        const uint32_t err = (a > b) ? (a - b) : (b - a);
        max_abs_error = std::max(max_abs_error, err);
        sum_abs_error += err;
    }

    const double mean_abs_error =
        static_cast<double>(sum_abs_error) / static_cast<double>(input.size());
    const double cratio = static_cast<double>(array->sc->nbytes) /
                          static_cast<double>(array->sc->cbytes);

    std::cout << "{\n"
              << "  \"codec\": \"htj2k\",\n"
              << "  \"codec_id\": " << codec_id << ",\n"
              << "  \"codec_meta\": " << codec_meta << ",\n"
              << "  \"target_cratio\": "
              << ((codec_meta != 0) ? std::to_string(codec_meta / 10.0) : "null") << ",\n"
              << "  \"input_nbytes\": " << array->sc->nbytes << ",\n"
              << "  \"compressed_cbytes\": " << array->sc->cbytes << ",\n"
              << "  \"achieved_cratio\": " << cratio << ",\n"
              << "  \"max_abs_error\": " << max_abs_error << ",\n"
              << "  \"mean_abs_error\": " << mean_abs_error << "\n"
              << "}\n";

    if (codec_meta == 0 && max_abs_error != 0) {
        fail("lossless roundtrip was not exact");
    }

    check(b2nd_free(array), "b2nd_free");
    check(b2nd_free_ctx(ctx), "b2nd_free_ctx");
    blosc2_destroy();
    return 0;
}
