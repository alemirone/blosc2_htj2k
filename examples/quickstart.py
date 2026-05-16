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


def main():
    parser = argparse.ArgumentParser(description="Hands-on blosc2_htj2k roundtrip")
    parser.add_argument("--backend", default=None, help="backend name, for example openhtj2k or kakadu")
    parser.add_argument("--lossy", action="store_true", help="enable codec_meta rate mode")
    parser.add_argument("--codec-meta", type=int, default=80, help="lossy meta byte, interpreted as meta / 10.0")
    args = parser.parse_args()

    blosc2_htj2k.register_codec()
    blosc2_htj2k.configure(backend=args.backend)

    data = synthetic_uint16()
    cparams = {
        "codec": blosc2_htj2k.CODEC_ID,
        "filters": [],
        "splitmode": blosc2.SplitMode.NEVER_SPLIT,
    }
    if args.lossy:
        cparams["codec_meta"] = args.codec_meta

    compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
    decoded = compressed[...]
    error = np.abs(decoded.astype(np.int32) - data.astype(np.int32))

    payload = {
        "codec": blosc2_htj2k.CODEC_NAME,
        "codec_id": blosc2_htj2k.CODEC_ID,
        "backend": args.backend or "manifest/default",
        "lossy": args.lossy,
        "codec_meta": cparams.get("codec_meta", 0),
        "input_nbytes": int(data.nbytes),
        "compressed_cbytes": int(compressed.schunk.cbytes),
        "equal": bool(np.array_equal(decoded, data)),
        "max_abs_error": int(error.max()),
        "mean_abs_error": float(error.mean()),
    }
    print(json.dumps(payload, indent=2, sort_keys=True))

    if args.lossy:
        assert not payload["equal"]
        assert payload["max_abs_error"] <= 512
        assert payload["mean_abs_error"] <= 80.0
    else:
        np.testing.assert_array_equal(decoded, data)


if __name__ == "__main__":
    main()
