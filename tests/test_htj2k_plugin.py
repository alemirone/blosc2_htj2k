import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import subprocess
import sys

import blosc2
import numpy as np
import pytest

try:
    import blosc2_htj2k
except RuntimeError as exc:
    if "official 'htj2k' codec id" in str(exc):
        pytest.skip("requires patched python-blosc2 registry", allow_module_level=True)
    raise


def _load_blosc2_shared_library():
    root = Path(blosc2.__file__).resolve().parent
    candidates = []
    libdir = root / "lib"
    if sys.platform.startswith("linux"):
        candidates.extend([libdir / "libblosc2.so", *sorted(libdir.glob("libblosc2.so*"))])
    elif sys.platform == "darwin":
        candidates.extend([libdir / "libblosc2.dylib", *sorted(libdir.glob("libblosc2.*.dylib"))])
    elif os.name == "nt":
        candidates.extend([root / "blosc2.dll", libdir / "blosc2.dll", libdir / "libblosc2.dll"])
    for candidate in candidates:
        if candidate.exists():
            return ctypes.CDLL(str(candidate))
    found = ctypes.util.find_library("blosc2")
    if found:
        return ctypes.CDLL(found)
    raise FileNotFoundError("Could not locate libblosc2 for registry checks")


def test_htj2k_global_registry_id_with_patched_python_blosc2():
    if os.environ.get("BLOSC2_EXPECT_GLOBAL_CODEC_IDS") != "1":
        pytest.skip("global codec-id check only runs with patched python-blosc2")
    lib = _load_blosc2_shared_library()
    lib.blosc2_compname_to_compcode.argtypes = [ctypes.c_char_p]
    lib.blosc2_compname_to_compcode.restype = ctypes.c_int
    compcode = lib.blosc2_compname_to_compcode(b"htj2k")
    assert compcode == blosc2_htj2k.CODEC_ID == 40


def _last_json_line(text):
    for line in reversed(text.splitlines()):
        if line.strip().startswith("{"):
            return line
    raise AssertionError(f"no JSON payload found in output:\n{text}")


def test_htj2k_manifest_and_listing():
    diag = blosc2_htj2k.diagnose()
    assert diag["manifest_priority"]["htj2k"] == ["kakadu", "openhtj2k"]
    plugins = blosc2_htj2k.list_plugins()
    assert "plugins" in plugins
    assert any(plugin.get("family") == "htj2k" and plugin.get("backend") == "grok"
               for plugin in plugins["plugins"])


def test_htj2k_grok_backend_roundtrip():
    code = r"""
import json

import blosc2
import blosc2_htj2k
import numpy as np

if "grok" not in blosc2_htj2k.available_backends()["htj2k"]:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend="grok")
base = np.arange(48 * 64, dtype=np.uint32).reshape(48, 64)
data = ((base * 7) % 65536).astype(np.uint16)
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
decoded = compressed[...]
np.testing.assert_array_equal(decoded, data)
print(json.dumps({"skipped": False, "cbytes": int(compressed.schunk.cbytes)}))
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("Grok HTJ2K backend is not installed")


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

backends = [backend for backend in blosc2_htj2k.available_backends()["htj2k"] if backend != "grok"]
if not backends:
    print(json.dumps({"skipped": True, "reason": "no lossy-capable HTJ2K backend installed"}))
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


def test_htj2k_uint32_lossless_roundtrip_with_kakadu_if_available():
    code = r"""
import json

import blosc2_htj2k

if "kakadu" not in blosc2_htj2k.available_backends()["htj2k"]:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)

import blosc2
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend="kakadu")

base = np.arange(48 * 64, dtype=np.uint32).reshape(48, 64)
data = ((base * np.uint32(104729)) ^ (base << np.uint32(16)) ^ np.uint32(0x80000000)).astype(np.uint32)
data[0, 0] = np.uint32(0)
data[0, 1] = np.iinfo(np.uint32).max
data[0, 2] = np.uint32(2**31)
data[0, 3] = np.uint32(2**31 - 1)

cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
decoded = compressed[...]
np.testing.assert_array_equal(decoded, data)
print(json.dumps({
    "skipped": False,
    "dtype": str(decoded.dtype),
    "min": int(decoded.min()),
    "max": int(decoded.max()),
}))
"""
    proc = subprocess.run(
        [sys.executable, "-c", code],
        text=True,
        capture_output=True,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("Kakadu HTJ2K backend is not installed")
    assert payload["dtype"] == "uint32"
    assert payload["max"] == 2**32 - 1


def test_htj2k_float32_uint16_roundtrip_with_available_backend():
    code = r"""
import json

import blosc2_htj2k

backends = blosc2_htj2k.available_backends()["htj2k"]
if not backends:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)
backend = "openhtj2k" if "openhtj2k" in backends else backends[0]

import blosc2
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend=backend, float_mode="uint16")

y, x = np.mgrid[0:96, 0:128]
data = (0.25 * np.sin(x / 7.0) + 0.75 * np.cos(y / 11.0) + x * 0.001).astype(np.float32)
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
decoded = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)[...]
max_abs = float(np.max(np.abs(decoded - data)))
bound = float((data.max() - data.min()) / (2 * 65535) + 2e-6)
assert decoded.dtype == np.float32
assert decoded.shape == data.shape
assert max_abs <= bound
print(json.dumps({"skipped": False, "backend": backend, "max_abs": max_abs, "bound": bound}))
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("no HTJ2K backend installed")
    assert payload["max_abs"] <= payload["bound"]


def test_htj2k_float32_uint8_clamp_constant_and_nan_behaviour():
    code = r"""
import json

import blosc2_htj2k

backends = blosc2_htj2k.available_backends()["htj2k"]
if not backends:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)
backend = "openhtj2k" if "openhtj2k" in backends else backends[0]

import blosc2
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(
    backend=backend,
    float_mode="uint8",
    float_clamp_min=-1.0,
    float_clamp_max=1.0,
)

data = np.linspace(-2.0, 2.0, 64 * 64, dtype=np.float32).reshape(64, 64)
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
decoded = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)[...]
assert decoded.dtype == np.float32
assert float(decoded.min()) >= -1.00001
assert float(decoded.max()) <= 1.00001
assert abs(float(decoded[0, 0]) + 1.0) <= 1e-6
assert abs(float(decoded[-1, -1]) - 1.0) <= 1e-6

constant = np.full((16, 16), 3.5, dtype=np.float32)
constant_decoded = blosc2.asarray(constant, chunks=constant.shape, blocks=constant.shape, cparams=cparams)[...]
np.testing.assert_array_equal(constant_decoded, np.full_like(constant, 1.0))

bad = data.copy()
bad[3, 4] = np.nan
failed = False
try:
    blosc2.asarray(bad, chunks=bad.shape, blocks=bad.shape, cparams=cparams)[...]
except Exception as exc:
    failed = True
assert failed
print(json.dumps({"skipped": False, "backend": backend}))
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("no HTJ2K backend installed")


def test_htj2k_float32_constant_chunk_shortcut():
    code = r"""
import json

import blosc2_htj2k

backends = blosc2_htj2k.available_backends()["htj2k"]
if not backends:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)
backend = "openhtj2k" if "openhtj2k" in backends else backends[0]

import blosc2
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend=backend, float_mode="uint8")

data = np.full((16, 16), 3.5, dtype=np.float32)
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
decoded = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)[...]
np.testing.assert_array_equal(decoded, data)
print(json.dumps({"skipped": False, "backend": backend}))
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("no HTJ2K backend installed")


def test_htj2k_float32_disabled_fails_clearly():
    code = r"""
import json

import blosc2_htj2k

backends = blosc2_htj2k.available_backends()["htj2k"]
if not backends:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)
backend = backends[0]

import blosc2
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend=backend)
data = np.arange(32 * 32, dtype=np.float32).reshape(32, 32)
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    if proc.returncode == 0 and '"skipped": true' in proc.stdout.lower():
        pytest.skip("no HTJ2K backend installed")
    assert proc.returncode != 0
    assert "float32 input requires opt-in float mode" in (proc.stdout + proc.stderr)


def test_htj2k_float_diagnostics_show_config():
    code = r"""
import json
import blosc2_htj2k

blosc2_htj2k.configure(backend="openhtj2k", float_mode="uint16", float_clamp_min=0.0)
diag = blosc2_htj2k.diagnose()
assert diag["float_config"]["enabled"] is True
assert diag["float_config"]["quant_bits"] == 16
assert diag["float_config"]["clamp_min_set"] is True
print(json.dumps({"ok": True}))
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_htj2k_float32_uint32_with_kakadu_if_available():
    code = r"""
import json

import blosc2_htj2k

if "kakadu" not in blosc2_htj2k.available_backends()["htj2k"]:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)

import blosc2
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend="kakadu", float_mode="uint32")

data = np.linspace(-123.25, 456.75, 48 * 64, dtype=np.float32).reshape(48, 64)
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
decoded = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)[...]
max_abs = float(np.max(np.abs(decoded - data)))
assert decoded.dtype == np.float32
assert max_abs <= 2e-4
print(json.dumps({"skipped": False, "max_abs": max_abs}))
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("Kakadu HTJ2K backend is not installed")
    assert payload["max_abs"] <= 2e-4


def test_htj2k_float32_uint32_lossy_with_kakadu_if_available():
    code = r"""
import json

import blosc2_htj2k

if "kakadu" not in blosc2_htj2k.available_backends()["htj2k"]:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)

import blosc2
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend="kakadu", float_mode="uint32")

y, x = np.mgrid[0:96, 0:128]
data = (0.25 * np.sin(x / 7) + 0.75 * np.cos(y / 11) + x * 0.001).astype(np.float32)
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "codec_meta": 80,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
decoded = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)[...]
err = np.abs(decoded - data)
max_abs = float(np.max(err))
mean_abs = float(np.mean(err))
assert decoded.dtype == np.float32
assert max_abs <= 1e-3
print(json.dumps({"skipped": False, "max_abs": max_abs, "mean_abs": mean_abs}))
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("Kakadu HTJ2K backend is not installed")
    assert payload["max_abs"] <= 1e-3


def test_htj2k_hdf5_roundtrip_with_numeric_filter_if_registry_enabled():
    if os.environ.get("BLOSC2_EXPECT_GLOBAL_CODEC_IDS") != "1":
        pytest.skip("HDF5 registry-aware check only runs with patched python-blosc2")
    code = r"""
import json
import tempfile

import blosc2_htj2k

backends = blosc2_htj2k.available_backends()["htj2k"]
if not backends:
    print(json.dumps({"skipped": True}))
    raise SystemExit(0)
backend = "openhtj2k" if "openhtj2k" in backends else backends[0]

import blosc2
import h5py
import hdf5plugin
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend=backend)
hdf5plugin.register("blosc2", force=True)

y, x = np.mgrid[0:32, 0:48]
base = (22000 + 3000 * np.sin(x / 5.0) + 2000 * np.cos(y / 7.0)).clip(0, 65535).astype(np.uint16)
data = np.stack([base, (base + 17).astype(np.uint16)], axis=0)
chunks = (1,) + data.shape[1:]
compression_opts = (
    0,
    0,
    0,
    0,
    5,
    hdf5plugin.Blosc2.NOFILTER,
    int(blosc2_htj2k.CODEC_ID),
)
with tempfile.TemporaryDirectory() as tmpdir:
    fn = f"{tmpdir}/htj2k_numeric_filter.h5"
    with h5py.File(fn, "w") as h5f:
        h5f.create_dataset(
            "entry/data",
            data=data,
            chunks=chunks,
            compression=hdf5plugin.BLOSC2_ID,
            compression_opts=compression_opts,
        )
    with h5py.File(fn, "r") as h5f:
        dset = h5f["entry/data"]
        info = dset.id.get_chunk_info_by_coord((0, 0, 0))
        decoded = dset[...]
    np.testing.assert_array_equal(decoded, data)
    assert info.filter_mask == 0
    print(json.dumps({"skipped": False, "backend": backend, "shape": list(decoded.shape), "dtype": str(decoded.dtype)}))
"""
    proc = subprocess.run([sys.executable, "-c", code], text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    payload = json.loads(_last_json_line(proc.stdout))
    if payload["skipped"]:
        pytest.skip("No HTJ2K backend is installed")
    assert payload["dtype"] == "uint16"


def test_htj2k_cli_diagnose():
    proc = subprocess.run(
        [sys.executable, "-m", "blosc2_htj2k", "--diagnose"],
        text=True,
        capture_output=True,
        check=True,
    )
    payload = json.loads(proc.stdout)
    assert payload["manifest_priority"]["htj2k"] == ["kakadu", "openhtj2k"]
