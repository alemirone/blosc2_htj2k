#!/usr/bin/env python3
import argparse
import json
from pathlib import Path

import blosc2
import blosc2_htj2k
import hdf5plugin
import h5py
import numpy as np


def synthetic_uint16(shape=(128, 128)):
    y, x = np.mgrid[0 : shape[0], 0 : shape[1]]
    return (
        22000
        + 9000 * np.sin(x / 5.0)
        + 7000 * np.cos(y / 9.0)
        + ((x * y) % 2048)
    ).clip(0, 65535).astype(np.uint16)


def synthetic_float32(shape=(128, 128)):
    y, x = np.mgrid[0 : shape[0], 0 : shape[1]]
    return (
        np.sin(x / 7.0)
        + 0.5 * np.cos(y / 11.0)
        + 0.001 * (x - y)
    ).astype(np.float32)


def synthetic_stack(float_mode=False, nframes=4, shape=(64, 96)):
    if float_mode:
        base = synthetic_float32(shape)
        return np.stack([base + np.float32(0.01 * i) for i in range(nframes)]).astype(np.float32)
    base = synthetic_uint16(shape).astype(np.uint32)
    return np.stack([(base + i) % 65536 for i in range(nframes)]).astype(np.uint16)


def expected_float_target(data, clamp_min, clamp_max):
    expected = data.astype(np.float32, copy=True)
    if clamp_min is not None:
        expected = np.maximum(expected, np.float32(clamp_min))
    if clamp_max is not None:
        expected = np.minimum(expected, np.float32(clamp_max))
    return expected


def quantization_bound(expected, float_mode):
    qbits = int(float_mode.replace("uint", ""))
    qmax = float((1 << qbits) - 1) if qbits < 32 else float(0xFFFFFFFF)
    span = float(expected.max() - expected.min())
    return span / (2.0 * qmax) + 2e-6


def assert_hdf5_filter_was_applied(filename, dataset_name):
    with h5py.File(filename, "r") as h5f:
        dset = h5f[dataset_name]
        raw_chunk_nbytes = int(np.prod(dset.chunks) * dset.dtype.itemsize)
        skipped = []
        for iframe in range(dset.shape[0]):
            coord = (iframe,) + (0,) * (dset.ndim - 1)
            info = dset.id.get_chunk_info_by_coord(coord)
            if info.filter_mask != 0:
                skipped.append((coord, info.filter_mask, info.size, raw_chunk_nbytes))

    if skipped:
        lines = [
            "HDF5 wrote raw chunks because the Blosc2/HTJ2K filter was skipped.",
            "The decoded values can still look correct, but this file is not HTJ2K-compressed.",
            f"Make sure codec {blosc2_htj2k.CODEC_ID} is known by the Blosc2 runtime used by the HDF5 filter.",
            "Skipped chunks:",
        ]
        lines.extend(
            f"  coord={coord} filter_mask={mask} stored={stored} raw={raw}"
            for coord, mask, stored, raw in skipped
        )
        raise RuntimeError("\n".join(lines))


def write_hdf5_stack_pair(data, output_dir, codec_id):
    # Force hdf5plugin to load libh5blosc2 through its Python registration path.
    # If HDF5_PLUGIN_PATH is set, HDF5 can otherwise load the plugin first and
    # bypass hdf5plugin's init_filter() call.
    hdf5plugin.register("blosc2", force=True)

    output_dir.mkdir(parents=True, exist_ok=True)
    dataset_name = "entry/data"
    raw_file = output_dir / "htj2k_stack_raw.h5"
    compressed_file = output_dir / "htj2k_stack_blosc2_htj2k.h5"
    chunks = (1,) + tuple(data.shape[1:])
    compression_opts = (
        0,                              # reserved
        0,                              # reserved
        0,                              # reserved
        0,                              # reserved
        5,                              # clevel
        hdf5plugin.Blosc2.NOFILTER,     # Blosc2 prefilter
        codec_id,                       # htj2k codec id
    )

    with h5py.File(raw_file, "w") as h5f:
        h5f.create_dataset(dataset_name, data=data)

    with h5py.File(compressed_file, "w") as h5f:
        h5f.create_dataset(
            dataset_name,
            data=data,
            chunks=chunks,
            compression=hdf5plugin.BLOSC2_ID,
            compression_opts=compression_opts,
        )

    assert_hdf5_filter_was_applied(compressed_file, dataset_name)
    with h5py.File(compressed_file, "r") as h5f:
        decoded = h5f[dataset_name][...]

    return {
        "raw_file": str(raw_file.resolve()),
        "compressed_file": str(compressed_file.resolve()),
        "dataset": dataset_name,
        "chunks": chunks,
        "raw_file_size": raw_file.stat().st_size,
        "compressed_file_size": compressed_file.stat().st_size,
        "decoded": decoded,
    }


def main():
    parser = argparse.ArgumentParser(description="Hands-on blosc2_htj2k roundtrip")
    parser.add_argument("--backend", default=None, help="backend name, for example openhtj2k or kakadu")
    parser.add_argument("--lossy", action="store_true", help="enable codec_meta rate mode")
    parser.add_argument("--codec-meta", type=int, default=80, help="lossy meta byte, interpreted as meta / 10.0")
    parser.add_argument("--float-mode", choices=("uint8", "uint16", "uint32"), help="quantize float32 chunks before HTJ2K compression")
    parser.add_argument("--float-clamp-min", type=float, default=None, help="optional float32 saturation lower bound")
    parser.add_argument("--float-clamp-max", type=float, default=None, help="optional float32 saturation upper bound")
    parser.add_argument("--output-dir", default="quickstart_output", help="directory for the raw and compressed HDF5 examples")
    args = parser.parse_args()

    blosc2_htj2k.register_codec()
    blosc2_htj2k.configure(
        backend=args.backend,
        float_mode=args.float_mode,
        float_clamp_min=args.float_clamp_min,
        float_clamp_max=args.float_clamp_max,
    )

    data = synthetic_stack(float_mode=bool(args.float_mode))
    cparams = {
        "codec": blosc2_htj2k.CODEC_ID,
        "filters": [],
        "splitmode": blosc2.SplitMode.NEVER_SPLIT,
    }
    if args.lossy:
        cparams["codec_meta"] = args.codec_meta

    chunks = (1,) + tuple(data.shape[1:])
    compressed = blosc2.asarray(data, chunks=chunks, blocks=chunks, cparams=cparams)
    decoded = compressed[...]
    if args.float_mode:
        expected = expected_float_target(data, args.float_clamp_min, args.float_clamp_max)
        error = np.abs(decoded.astype(np.float32) - expected)
        equal = bool(np.array_equal(decoded, expected))
        max_abs_error = float(error.max())
        mean_abs_error = float(error.mean())
        bound = quantization_bound(expected, args.float_mode)
    else:
        expected = data
        error = np.abs(decoded.astype(np.int32) - data.astype(np.int32))
        equal = bool(np.array_equal(decoded, data))
        max_abs_error = int(error.max())
        mean_abs_error = float(error.mean())
        bound = None

    hdf5_demo = write_hdf5_stack_pair(data, Path(args.output_dir), blosc2_htj2k.CODEC_ID)
    hdf5_decoded = hdf5_demo.pop("decoded")
    if args.float_mode:
        hdf5_expected = expected_float_target(data, args.float_clamp_min, args.float_clamp_max)
        hdf5_error = np.abs(hdf5_decoded.astype(np.float32) - hdf5_expected)
        hdf5_max_abs_error = float(hdf5_error.max())
        hdf5_mean_abs_error = float(hdf5_error.mean())
    else:
        hdf5_expected = data
        hdf5_error = np.abs(hdf5_decoded.astype(np.int32) - data.astype(np.int32))
        hdf5_max_abs_error = int(hdf5_error.max())
        hdf5_mean_abs_error = float(hdf5_error.mean())

    payload = {
        "codec": blosc2_htj2k.CODEC_NAME,
        "codec_id": blosc2_htj2k.CODEC_ID,
        "backend": args.backend or "manifest/default",
        "lossy": args.lossy,
        "float_mode": args.float_mode or "off",
        "codec_meta": cparams.get("codec_meta", 0),
        "input_nbytes": int(data.nbytes),
        "compressed_cbytes": int(compressed.schunk.cbytes),
        "equal": equal,
        "max_abs_error": max_abs_error,
        "mean_abs_error": mean_abs_error,
        "expected_quant_bound": bound,
        "hdf5_raw_file": hdf5_demo["raw_file"],
        "hdf5_compressed_file": hdf5_demo["compressed_file"],
        "hdf5_dataset": hdf5_demo["dataset"],
        "hdf5_chunks": hdf5_demo["chunks"],
        "hdf5_raw_file_size": hdf5_demo["raw_file_size"],
        "hdf5_compressed_file_size": hdf5_demo["compressed_file_size"],
        "hdf5_max_abs_error": hdf5_max_abs_error,
        "hdf5_mean_abs_error": hdf5_mean_abs_error,
    }
    print(f"Wrote raw HDF5 stack: {payload['hdf5_raw_file']}::{payload['hdf5_dataset']}")
    print(f"Wrote compressed HDF5 stack: {payload['hdf5_compressed_file']}::{payload['hdf5_dataset']}")
    print(json.dumps(payload, indent=2, sort_keys=True))

    if args.float_mode:
        if not args.lossy:
            assert payload["max_abs_error"] <= payload["expected_quant_bound"]
            assert payload["hdf5_max_abs_error"] <= payload["expected_quant_bound"]
    elif args.lossy:
        assert not payload["equal"]
        assert payload["max_abs_error"] <= 512
        assert payload["mean_abs_error"] <= 80.0
    else:
        np.testing.assert_array_equal(decoded, expected)
        np.testing.assert_array_equal(hdf5_decoded, hdf5_expected)


if __name__ == "__main__":
    main()
