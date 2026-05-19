#include <b2nd.h>
#include <blosc2.h>
#include <hdf5.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef BLOSC_CODEC_HTJ2K
#define BLOSC_CODEC_HTJ2K 40
#endif

namespace {

constexpr H5Z_filter_t kBlosc2Filter = 32026;
constexpr const char *kDatasetName = "/entry/data";
constexpr int kFrames = 4;
constexpr int kHeight = 64;
constexpr int kWidth = 96;
constexpr int64_t kChunkItems = kHeight * kWidth;
constexpr int64_t kChunkNbytes = kChunkItems * static_cast<int64_t>(sizeof(uint16_t));

struct Options {
    int codec_meta = 0;
    std::string output_dir = "quickstart_output";
};

struct H5Handle {
    hid_t id = -1;
    herr_t (*close_fn)(hid_t) = nullptr;

    H5Handle() = default;
    H5Handle(hid_t id_, herr_t (*close_fn_)(hid_t)) : id(id_), close_fn(close_fn_) {}
    H5Handle(const H5Handle &) = delete;
    H5Handle &operator=(const H5Handle &) = delete;
    H5Handle(H5Handle &&other) noexcept : id(other.id), close_fn(other.close_fn) {
        other.id = -1;
        other.close_fn = nullptr;
    }
    H5Handle &operator=(H5Handle &&other) noexcept {
        if (this != &other) {
            close();
            id = other.id;
            close_fn = other.close_fn;
            other.id = -1;
            other.close_fn = nullptr;
        }
        return *this;
    }
    ~H5Handle() {
        close();
    }
    void close() {
        if (id >= 0 && close_fn != nullptr) {
            close_fn(id);
        }
        id = -1;
    }
};

void fail(const std::string &message) {
    std::cerr << "ERROR: " << message << "\n";
    std::exit(1);
}

hid_t check_id(hid_t id, const std::string &what) {
    if (id < 0) {
        fail(what + " failed");
    }
    return id;
}

void check(int64_t rc, const std::string &what) {
    if (rc < 0) {
        fail(what + " failed with code " + std::to_string(rc));
    }
}

struct PluginLoadingGuard {
    unsigned int previous = H5PL_ALL_PLUGIN;
    bool restore = false;

    explicit PluginLoadingGuard(unsigned int state) {
        if (H5PLget_loading_state(&previous) >= 0) {
            restore = true;
        }
        check(H5PLset_loading_state(state), "H5PLset_loading_state");
    }
    PluginLoadingGuard(const PluginLoadingGuard &) = delete;
    PluginLoadingGuard &operator=(const PluginLoadingGuard &) = delete;
    ~PluginLoadingGuard() {
        if (restore) {
            H5PLset_loading_state(previous);
        }
    }
};

std::vector<uint16_t> make_stack() {
    std::vector<uint16_t> data(kFrames * kHeight * kWidth);
    for (int z = 0; z < kFrames; ++z) {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const auto value = static_cast<uint32_t>(
                    22000 + ((x * 37 + y * 113 + z * 251) % 17000));
                data[(z * kHeight + y) * kWidth + x] = static_cast<uint16_t>(value);
            }
        }
    }
    return data;
}

Options parse_options(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--codec-meta" && i + 1 < argc) {
            options.codec_meta = std::atoi(argv[++i]);
        } else if (arg == "--output-dir" && i + 1 < argc) {
            options.output_dir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--codec-meta N] [--output-dir DIR]\n"
                      << "  Writes a raw HDF5 stack and an HTJ2K-compressed HDF5 stack.\n"
                      << "  codec_meta=0 is lossless; non-zero means target cratio=N/10.\n";
            std::exit(0);
        } else {
            fail("unknown argument: " + arg);
        }
    }
    if (options.codec_meta < 0 || options.codec_meta > 255) {
        fail("--codec-meta must be in [0, 255]");
    }
    return options;
}

H5Handle create_file_with_entry_group(const std::string &filename) {
    H5Handle file(check_id(H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT),
                           "H5Fcreate"),
                  H5Fclose);
    H5Handle group(check_id(H5Gcreate2(file.id, "/entry", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                            "H5Gcreate2"),
                   H5Gclose);
    return file;
}

void write_raw_hdf5(const std::string &filename, const std::vector<uint16_t> &data) {
    const hsize_t dims[3] = {kFrames, kHeight, kWidth};
    H5Handle file = create_file_with_entry_group(filename);
    H5Handle space(check_id(H5Screate_simple(3, dims, nullptr), "H5Screate_simple"), H5Sclose);
    H5Handle dset(check_id(H5Dcreate2(file.id, kDatasetName, H5T_NATIVE_UINT16, space.id,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                            "H5Dcreate2"),
                  H5Dclose);
    check(H5Dwrite(dset.id, H5T_NATIVE_UINT16, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data()),
          "H5Dwrite raw");
}

std::vector<uint8_t> compress_frame(const uint16_t *frame, int codec_id, int codec_meta) {
    const int8_t ndim = 3;
    int64_t shape[] = {1, kHeight, kWidth};
    int32_t chunkshape[] = {1, kHeight, kWidth};
    int32_t blockshape[] = {1, kHeight, kWidth};

    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.compcode = static_cast<uint8_t>(codec_id);
    cparams.compcode_meta = static_cast<uint8_t>(codec_meta);
    cparams.clevel = 5;
    cparams.typesize = static_cast<int32_t>(sizeof(uint16_t));
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
    check(b2nd_from_cbuffer(ctx, &array, frame, kChunkNbytes), "b2nd_from_cbuffer");

    uint8_t *cframe = nullptr;
    int64_t cframe_len = 0;
    bool needs_free = false;
    check(b2nd_to_cframe(array, &cframe, &cframe_len, &needs_free), "b2nd_to_cframe");
    std::vector<uint8_t> out(cframe, cframe + cframe_len);

    if (needs_free) {
        free(cframe);
    }
    check(b2nd_free(array), "b2nd_free");
    check(b2nd_free_ctx(ctx), "b2nd_free_ctx");
    return out;
}

void write_compressed_hdf5(const std::string &filename, const std::vector<uint16_t> &data,
                           int codec_id, int codec_meta) {
    // The example writes already-compressed chunks, so the process does not need
    // an HDF5 Blosc2 plugin at write time.  This avoids accidental ABI mixing
    // with a system HDF5 different from the reader's HDF5.
    PluginLoadingGuard disable_plugins(0);

    const hsize_t dims[3] = {kFrames, kHeight, kWidth};
    const hsize_t chunks[3] = {1, kHeight, kWidth};
    const unsigned cd_values[11] = {
        0,                         // filter revision, filled by readers with set_local
        0,                         // block size, auto
        sizeof(uint16_t),           // type size
        static_cast<unsigned>(kChunkNbytes),
        5,                         // clevel
        0,                         // no Blosc2 prefilter
        static_cast<unsigned>(codec_id),
        3,                         // B2ND chunk rank
        1,
        kHeight,
        kWidth,
    };

    H5Handle file = create_file_with_entry_group(filename);
    H5Handle space(check_id(H5Screate_simple(3, dims, nullptr), "H5Screate_simple"), H5Sclose);
    H5Handle dcpl(check_id(H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate"), H5Pclose);
    check(H5Pset_chunk(dcpl.id, 3, chunks), "H5Pset_chunk");
    check(H5Pset_filter(dcpl.id, kBlosc2Filter, H5Z_FLAG_OPTIONAL, 11, cd_values), "H5Pset_filter");
    H5Handle dset(check_id(H5Dcreate2(file.id, kDatasetName, H5T_NATIVE_UINT16, space.id,
                                      H5P_DEFAULT, dcpl.id, H5P_DEFAULT),
                            "H5Dcreate2 compressed"),
                  H5Dclose);

    for (hsize_t z = 0; z < static_cast<hsize_t>(kFrames); ++z) {
        const uint16_t *frame = data.data() + z * kChunkItems;
        std::vector<uint8_t> cframe = compress_frame(frame, codec_id, codec_meta);
        const hsize_t offset[3] = {z, 0, 0};
        check(H5Dwrite_chunk(dset.id, H5P_DEFAULT, 0, offset, cframe.size(), cframe.data()),
              "H5Dwrite_chunk");
    }
}

struct Readback {
    std::vector<uint16_t> data;
    uint64_t stored_chunk_bytes = 0;
};

Readback read_hdf5_stack_transparently(const std::string &filename) {
    Readback out;
    out.data.resize(kFrames * kHeight * kWidth);

    PluginLoadingGuard enable_plugins(H5PL_ALL_PLUGIN);
    H5Handle file(check_id(H5Fopen(filename.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), "H5Fopen"),
                  H5Fclose);
    H5Handle dset(check_id(H5Dopen2(file.id, kDatasetName, H5P_DEFAULT), "H5Dopen2"), H5Dclose);

    check(H5Dread(dset.id, H5T_NATIVE_UINT16, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data.data()),
          "transparent H5Dread");

    for (hsize_t z = 0; z < static_cast<hsize_t>(kFrames); ++z) {
        const hsize_t offset[3] = {z, 0, 0};
        unsigned filter_mask = 0;
        haddr_t address = HADDR_UNDEF;
        hsize_t stored_size = 0;
        check(H5Dget_chunk_info_by_coord(dset.id, offset, &filter_mask, &address, &stored_size),
              "H5Dget_chunk_info_by_coord");
        if (filter_mask != 0) {
            fail("HDF5 chunk was written with the HTJ2K filter marked as skipped");
        }
        out.stored_chunk_bytes += static_cast<uint64_t>(stored_size);
    }
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    const Options options = parse_options(argc, argv);

    blosc2_init();
    const int codec_id = blosc2_compname_to_compcode("htj2k");
    if (codec_id != BLOSC_CODEC_HTJ2K) {
        fail("updated c-blosc2 with registered codec name 'htj2k' was not found");
    }

    std::filesystem::create_directories(options.output_dir);
    const std::string raw_file = options.output_dir + "/htj2k_stack_raw_cpp.h5";
    const std::string compressed_file = options.output_dir + "/htj2k_stack_blosc2_htj2k_cpp.h5";

    const auto input = make_stack();
    write_raw_hdf5(raw_file, input);
    write_compressed_hdf5(compressed_file, input, codec_id, options.codec_meta);
    const Readback readback = read_hdf5_stack_transparently(compressed_file);

    uint32_t max_abs_error = 0;
    uint64_t sum_abs_error = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        const uint32_t a = input[i];
        const uint32_t b = readback.data[i];
        const uint32_t err = (a > b) ? (a - b) : (b - a);
        max_abs_error = std::max(max_abs_error, err);
        sum_abs_error += err;
    }

    const uint64_t input_nbytes = static_cast<uint64_t>(input.size() * sizeof(uint16_t));
    const double mean_abs_error =
        static_cast<double>(sum_abs_error) / static_cast<double>(input.size());

    std::cout << "Wrote raw HDF5 stack: " << std::filesystem::absolute(raw_file).string()
              << "::" << kDatasetName << "\n";
    std::cout << "Wrote compressed HDF5 stack: "
              << std::filesystem::absolute(compressed_file).string()
              << "::" << kDatasetName << "\n";
    std::cout << "{\n"
              << "  \"codec\": \"htj2k\",\n"
              << "  \"codec_id\": " << codec_id << ",\n"
              << "  \"codec_meta\": " << options.codec_meta << ",\n"
              << "  \"target_cratio\": "
              << ((options.codec_meta != 0) ? std::to_string(options.codec_meta / 10.0) : "null") << ",\n"
              << "  \"input_nbytes\": " << input_nbytes << ",\n"
              << "  \"stored_chunk_bytes\": " << readback.stored_chunk_bytes << ",\n"
              << "  \"raw_file\": \"" << std::filesystem::absolute(raw_file).string() << "\",\n"
              << "  \"compressed_file\": \"" << std::filesystem::absolute(compressed_file).string() << "\",\n"
              << "  \"raw_file_size\": " << std::filesystem::file_size(raw_file) << ",\n"
              << "  \"compressed_file_size\": " << std::filesystem::file_size(compressed_file) << ",\n"
              << "  \"max_abs_error\": " << max_abs_error << ",\n"
              << "  \"mean_abs_error\": " << mean_abs_error << "\n"
              << "}\n";

    if (options.codec_meta == 0 && max_abs_error != 0) {
        fail("lossless HDF5 roundtrip was not exact");
    }

    blosc2_destroy();
    return 0;
}
