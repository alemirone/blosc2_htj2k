#!/usr/bin/env python3
import argparse
import json

import blosc2
import blosc2_htj2k
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


def main():
    parser = argparse.ArgumentParser(description="Hands-on blosc2_htj2k roundtrip")
    parser.add_argument("--backend", default=None, help="backend name, for example openhtj2k or kakadu")
    parser.add_argument("--lossy", action="store_true", help="enable codec_meta rate mode")
    parser.add_argument("--codec-meta", type=int, default=80, help="lossy meta byte, interpreted as meta / 10.0")
    parser.add_argument("--float-mode", choices=("uint8", "uint16", "uint32"), help="quantize float32 chunks before HTJ2K compression")
    parser.add_argument("--float-clamp-min", type=float, default=None, help="optional float32 saturation lower bound")
    parser.add_argument("--float-clamp-max", type=float, default=None, help="optional float32 saturation upper bound")
    args = parser.parse_args()

    blosc2_htj2k.register_codec()
    blosc2_htj2k.configure(
        backend=args.backend,
        float_mode=args.float_mode,
        float_clamp_min=args.float_clamp_min,
        float_clamp_max=args.float_clamp_max,
    )

    data = synthetic_float32() if args.float_mode else synthetic_uint16()
    cparams = {
        "codec": blosc2_htj2k.CODEC_ID,
        "filters": [],
        "splitmode": blosc2.SplitMode.NEVER_SPLIT,
    }
    if args.lossy:
        cparams["codec_meta"] = args.codec_meta

    compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
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
    }
    print(json.dumps(payload, indent=2, sort_keys=True))

    if args.float_mode:
        if not args.lossy:
            assert payload["max_abs_error"] <= payload["expected_quant_bound"]
    elif args.lossy:
        assert not payload["equal"]
        assert payload["max_abs_error"] <= 512
        assert payload["mean_abs_error"] <= 80.0
    else:
        np.testing.assert_array_equal(decoded, expected)


if __name__ == "__main__":
    main()
