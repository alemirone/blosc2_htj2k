import json
import subprocess
import sys

import blosc2
import blosc2_htj2k
import numpy as np
import pytest


def _last_json_line(text):
    for line in reversed(text.splitlines()):
        if line.strip().startswith("{"):
            return line
    raise AssertionError(f"no JSON payload found in output:\n{text}")


def test_htj2k_manifest_and_listing():
    diag = blosc2_htj2k.diagnose()
    assert diag["manifest_priority"]["htj2k"] == ["kakadu", "openhtj2k"]
    assert "plugins" in blosc2_htj2k.list_plugins()


def test_htj2k_roundtrip_with_available_backend():
    backends = blosc2_htj2k.available_backends()["htj2k"]
    if not backends:
        pytest.skip("no HTJ2K backend installed")
    backend = "kakadu" if "kakadu" in backends else backends[0]
    blosc2_htj2k.register_codec()
    blosc2_htj2k.configure(backend=backend)
    data = (np.arange(64 * 64, dtype=np.uint16).reshape(64, 64) % 4096)
    cparams = {
        "codec": blosc2_htj2k.CODEC_ID,
        "filters": [],
        "splitmode": blosc2.SplitMode.NEVER_SPLIT,
    }
    compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
    np.testing.assert_array_equal(compressed[...], data)


def test_htj2k_lossy_roundtrip_with_available_plugins():
    code = r"""
import json
import subprocess
import sys

import blosc2_htj2k

def last_json_line(text):
    for line in reversed(text.splitlines()):
        if line.strip().startswith("{"):
            return line
    raise RuntimeError(f"no JSON payload found in output:\n{text}")

backends = blosc2_htj2k.available_backends()["htj2k"]
if not backends:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)

results = []
for backend in backends:
    worker = f'''
import json

import blosc2
import blosc2_htj2k
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend={backend!r})

y, x = np.mgrid[0:128, 0:128]
data = (
    22000
    + 9000 * np.sin(x / 5.0)
    + 7000 * np.cos(y / 9.0)
    + ((x * y) % 2048)
).clip(0, 65535).astype(np.uint16)
cparams = {{
    "codec": blosc2_htj2k.CODEC_ID,
    "codec_meta": 80,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}}
compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
decoded = compressed[...]
error = np.abs(decoded.astype(np.int32) - data.astype(np.int32))
payload = {{
    "backend": {backend!r},
    "cbytes": int(compressed.schunk.cbytes),
    "nbytes": int(data.nbytes),
    "equal": bool(np.array_equal(decoded, data)),
    "max_abs": int(error.max()),
    "mean_abs": float(error.mean()),
}}
assert decoded.dtype == data.dtype
assert decoded.shape == data.shape
assert not payload["equal"]
assert payload["cbytes"] < payload["nbytes"]
assert payload["max_abs"] <= 512
assert payload["mean_abs"] <= 80.0
print(json.dumps(payload))
'''
    proc = subprocess.run([sys.executable, "-c", worker], text=True, capture_output=True)
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        raise SystemExit(proc.returncode)
    results.append(json.loads(last_json_line(proc.stdout)))
print(json.dumps({"skipped": False, "results": results}))
"""
    proc = subprocess.run(
        [sys.executable, "-c", code],
        text=True,
        capture_output=True,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("no HTJ2K backend installed")
    assert {result["backend"] for result in payload["results"]}


def test_htj2k_cli_diagnose():
    proc = subprocess.run(
        [sys.executable, "-m", "blosc2_htj2k", "--diagnose"],
        text=True,
        capture_output=True,
        check=True,
    )
    payload = json.loads(proc.stdout)
    assert payload["manifest_priority"]["htj2k"] == ["kakadu", "openhtj2k"]
