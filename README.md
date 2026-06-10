# blosc2_htj2k

`blosc2_htj2k` is a Blosc2 codec plugin for High Throughput JPEG2000 / HTJ2K
codestreams using official codec id `40`.

It is the HTJ2K-specific successor of the previous `blosc2_grok`
runtime-backend prototype.  The goal is to keep the Blosc2 codec
small and backend-agnostic: the Blosc2 codec is called `htj2k`, and the actual
HTJ2K implementation is selected through backend plugins installed inside the
package.

The package uses the official HTJ2K codec id in the c-blosc2 registry:

```text
codec name:     htj2k
codec id:       40
library:        libblosc2_htj2k.so
Python package: blosc2_htj2k
```

The codec is plugin-only.  Backend selection comes from explicit
configuration, environment variables, or the installed manifest.  A bundled
Grok HTJ2K backend is installed for explicit selection and fallback testing,
but it is not first in the default manifest.

The default manifest is:

```json
{
  "htj2k": ["kakadu", "openhtj2k"]
}
```

This means: use Kakadu when it is installed and loadable, otherwise use the
redistributable OpenHTJ2K backend.

Grok can also be selected explicitly:

```bash
export BLOSC2_HTJ2K_BACKEND=grok
```

or by editing a deployment manifest, for example:

```json
{
  "htj2k": ["kakadu", "openhtj2k", "grok"]
}
```

## Blosc2 Plugin Loading Principles

There are two independent plugin layers when HTJ2K data is read or written
through HDF5:

1. The HDF5 layer must find the Blosc2 HDF5 filter.  This filter owns HDF5
   filter id `32026` and is normally discovered with `HDF5_PLUGIN_PATH`.
2. The Blosc2 layer must then resolve codec id `40`, whose registry name is
   `htj2k`.

c-blosc2 does not start Python first.  The normal C/C++ loading path is:

1. Try to load the codec library directly with the platform dynamic loader
   (`libblosc2_htj2k.so`, `libblosc2_htj2k.dylib`, or `blosc2_htj2k.dll`).
   This uses the usual runtime library search paths: RPATH, system library
   directories, `LD_LIBRARY_PATH` on Linux, `DYLD_LIBRARY_PATH` on macOS where
   permitted, or `PATH` on Windows.
2. Only if that direct lookup fails, and only if `python` or `python3` is
   callable, c-blosc2 can run a short optional discovery helper:

   ```bash
   python -c "import blosc2_htj2k; blosc2_htj2k.print_libpath()"
   ```

   The Python process only reports the installed codec library path.  It does
   not perform the HDF5 read/write and it is not embedded into the C++ process.

Once `libblosc2_htj2k` is loaded, backend selection is handled by
`blosc2_htj2k` itself.  A normal wheel install uses the manifest installed next
to the library:

```text
blosc2_htj2k_plugins.json -> htj2k backend priority
```

For a standard installation, no `BLOSC2_HTJ2K_PLUGIN_PATH` is needed because
the backend plugins are installed under the default `plugins/` directory next
to `libblosc2_htj2k`.

The relevant environment variables are:

```bash
export HDF5_PLUGIN_PATH=/path/to/hdf5/plugins
export LD_LIBRARY_PATH=/path/to/site-packages/blosc2_htj2k:${LD_LIBRARY_PATH:-}
export BLOSC2_HTJ2K_PLUGIN_PATH=/path/to/site-packages/blosc2_htj2k/plugins
export BLOSC2_HTJ2K_BACKEND=openhtj2k
```

`HDF5_PLUGIN_PATH` is for HDF5 filter discovery.  `LD_LIBRARY_PATH` (or the
platform equivalent) is for the main codec library.  `BLOSC2_HTJ2K_PLUGIN_PATH`
and `BLOSC2_HTJ2K_BACKEND` are only for backend discovery/selection after the
codec library has already been loaded.

The direct search for `libblosc2_htj2k.so` follows the platform dynamic-loader
rules:

- Linux: RPATH/RUNPATH entries, `LD_LIBRARY_PATH`, the `ld.so` cache, and
  system library directories such as `/lib` and `/usr/lib`.
- macOS: install names, `@rpath`/`LC_RPATH`, `DYLD_LIBRARY_PATH`, and
  `DYLD_FALLBACK_LIBRARY_PATH`, subject to the usual macOS loader restrictions.
- Windows: the DLL search path, including the application directory, system
  directories, directories added with `AddDllDirectory()`/`SetDllDirectory()`,
  and `PATH`.

`BLOSC2_HTJ2K_PLUGIN_PATH` is not used to find `libblosc2_htj2k.so`; it is used
by `libblosc2_htj2k` after loading, to find backend plugin directories.

Minimal C++/HDF5 configuration in a Python environment where `python` or
`python3` is callable and can import the installed `blosc2_htj2k` package:

```bash
export HDF5_PLUGIN_PATH=/path/to/hdf5/plugins
./my_hdf5_reader_or_writer
```

In this mode HDF5 finds the Blosc2 filter, and c-blosc2 can ask Python where
the `blosc2_htj2k` codec library was installed if the dynamic loader cannot
find it directly.  With the standard wheel layout and the backend plugins
installed next to `libblosc2_htj2k`, `LD_LIBRARY_PATH`,
`BLOSC2_HTJ2K_PLUGIN_PATH`, and `BLOSC2_HTJ2K_BACKEND` are not required.

Minimal C++/HDF5 configuration for systems where Python must not be called:

```bash
export HDF5_PLUGIN_PATH=/path/to/hdf5/plugins
export LD_LIBRARY_PATH=/path/to/site-packages/blosc2_htj2k:${LD_LIBRARY_PATH:-}
./my_hdf5_reader_or_writer
```

Use `PATH` instead of `LD_LIBRARY_PATH` on Windows, and `DYLD_LIBRARY_PATH` on
macOS where it is permitted.  If backend plugins are not installed in the
default location next to `libblosc2_htj2k`, also set:

```bash
export BLOSC2_HTJ2K_PLUGIN_PATH=/path/to/site-packages/blosc2_htj2k/plugins
```

For Python programs, prefer explicit loading and configuration before opening
or creating HDF5 datasets:

```python
import hdf5plugin
import blosc2_htj2k
import h5py

blosc2_htj2k.configure()  # use the manifest priority

with h5py.File("data.h5", "r") as h5f:
    data = h5f["entry/data"][...]
```

Use `blosc2_htj2k.configure(backend="openhtj2k")`,
`blosc2_htj2k.configure(backend="grok")`, or
`blosc2_htj2k.configure(backend="kakadu")` only when a deployment needs to
force a backend and bypass the manifest priority.

If a Python session has already let HDF5 load the Blosc2 filter from
`HDF5_PLUGIN_PATH` before `import hdf5plugin`, force the Python-side HDF5 filter
registration:

```python
import hdf5plugin
hdf5plugin.register("blosc2", force=True)
```

Backend configuration must still happen before the first HTJ2K encode/decode
in the process.  After first codec use, `blosc2_htj2k.configure()` deliberately
refuses to change the runtime backend; restart the process or configure earlier.

## Plugin Philosophy

The package follows the same philosophy as the `blosc2_grok` prototype, but
with a narrower responsibility:

- `blosc2_htj2k` owns only the Blosc2 codec id and the HTJ2K dispatch logic.
- Each codec backend lives in a backend plugin directory.
- The core library does not directly depend on Kakadu.
- The backend ABI is a small C interface, so new HTJ2K backends can be added
  without changing the Blosc2-facing codec.
- Backend discovery is deterministic and inspectable.
- Python is convenient for configuration and diagnostics, but not required for
  C++/HDF5/web-service runtime when the bootstrap preload path is used.

The installed backend layout is:

```text
blosc2_htj2k/
  libblosc2_htj2k.so
  libblosc2_jpeg2000_bootstrap.so
  blosc2_htj2k_plugins.json
  plugins/
    htj2k/
      openhtj2k/
        libblosc2_openhtj2k_backend.so
        libopen_htj2k_R.so*
      grok/
        libblosc2_grok_htj2k_backend.so
      kakadu/
        libblosc2_kakadu_htj2k_backend.so
```

Backend capabilities:

| Backend | Built when | HTJ2K | `uint8` | `uint16` | `uint32` | Redistributable |
| --- | --- | --- | --- | --- | --- | --- |
| `plugins/htj2k/openhtj2k` | OpenHTJ2K `v0.4.0` API found or built | yes | yes | yes | no | yes |
| `plugins/htj2k/grok` | always | yes, lossless path | yes | yes | no | yes |
| `plugins/htj2k/kakadu` | Kakadu found | yes | yes | yes | yes | no |

Kakadu is optional and is not redistributed by this project.  The Grok backend
uses the bundled Grok implementation through the same HTJ2K plugin ABI as the
external backends; it is useful as an always-installed fallback, but OpenHTJ2K
remains the preferred redistributable backend for normal HTJ2K testing.

## Hands-On Quick Start

This plugin depends on `python-blosc2 >= 4.4.3`, which exposes the J2K/HTJ2K
codec IDs and ships a c-blosc2 runtime that knows the official registry entries.
The plugin uses the inline `b2nd_deserialize_meta_inline()` helper instead of
linking to the non-inline B2ND symbol.

This quickstart builds `hdf5plugin` from source and links its Blosc2 HDF5
filter against the same current c-blosc2 runtime installed by `python-blosc2`.
No new `hdf5plugin` codec names are needed: the examples pass low-level numeric
Blosc2 filter options (`cd_values`) with codec id `40`.  With that rebuilt
filter, c-blosc2 can discover the external `blosc2_htj2k` codec library at read
time, so the final HDF5 read does not need an explicit `import blosc2_htj2k`.

After the stack is installed, runtime setup is intentionally small.  For Python
programs, `import hdf5plugin` loads and registers the HDF5 Blosc2 filter, and
`LD_LIBRARY_PATH` only needs to expose the updated `libblosc2` runtime and the
`blosc2_htj2k` codec library:

```bash
export LD_LIBRARY_PATH=/path/to/blosc2/lib:/path/to/blosc2_htj2k:${LD_LIBRARY_PATH:-}
```

For non-Python HDF5 programs, command-line tools, services, or web servers, add
the HDF5 filter directory:

```bash
export HDF5_PLUGIN_PATH=/path/to/hdf5/plugins
export LD_LIBRARY_PATH=/path/to/blosc2/lib:/path/to/blosc2_htj2k:${LD_LIBRARY_PATH:-}
```

Keeping those library directories in `LD_LIBRARY_PATH` lets c-blosc2 load
`libblosc2_htj2k.so` directly.  It does not need to launch a Python interpreter
to ask where the codec library is.  No `BLOSC2_HTJ2K_BACKEND` variable is
required for the default installation because `blosc2_htj2k_plugins.json`
defines the backend priority.

The long block below bootstraps the current development stack from source.  It
is meant to be copy-pasted in a fresh terminal.

<details>
<summary>Copy-paste full source quickstart</summary>

```bash
workdir="$(mktemp -d -p /tmp blosc2_htj2k_quickstart_XXXXXXXX)"
cd "$workdir"
echo "Using quickstart directory: $PWD"

python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip setuptools wheel
python -m pip install scikit-build-core cython numpy pkgconfig py-cpuinfo h5py "blosc2>=4.4.3"
export CMAKE_BUILD_PARALLEL_LEVEL=20

export BLOSC2_PACKAGE="$(python -c 'from pathlib import Path; import blosc2; print(Path(blosc2.__file__).resolve().parent)')"
export LD_LIBRARY_PATH="${BLOSC2_PACKAGE}/lib:${LD_LIBRARY_PATH:-}"

# The pkg-config file inside the python-blosc2 wheel can contain build-time
# paths.  Write a local one so hdf5plugin links to this environment's c-blosc2.
mkdir -p "$PWD/pkgconfig"
python - <<'PY'
import os
from pathlib import Path

prefix = Path(os.environ["BLOSC2_PACKAGE"])
pc = Path.cwd() / "pkgconfig" / "blosc2.pc"
pc.write_text(f"""prefix={prefix}
exec_prefix=${{prefix}}
libdir=${{prefix}}/lib
includedir=${{prefix}}/include

Name: blosc2
Description: High performance meta-compressor optimized for binary data
Version: 3.1.3
Libs: -L${{libdir}} -l:libblosc2.so.8
Cflags: -I${{includedir}}
""")
PY
export PKG_CONFIG_PATH="$PWD/pkgconfig:${PKG_CONFIG_PATH:-}"

python - <<'PY'
import blosc2
print("blosc2:", blosc2.__version__)
print("J2K codec:", blosc2.Codec.J2K)
print("HTJ2K codec:", blosc2.Codec.HTJ2K)
PY

# Build hdf5plugin from source, but only its Blosc2 HDF5 filter, and link it to
# the c-blosc2 runtime installed by the blosc2 wheel above.
git clone https://github.com/silx-kit/hdf5plugin.git
rm -rf hdf5plugin/build hdf5plugin/src/hdf5plugin.egg-info
HDF5PLUGIN_SYSTEM_LIBRARIES=blosc2 \
HDF5PLUGIN_STRIP=blosc,bshuf,bzip2,fcidecomp,lz4,sperr,sz,sz3,zfp,zstd \
python -m pip install -v --no-build-isolation --force-reinstall --no-deps --no-cache-dir ./hdf5plugin

python - <<'PY'
import ctypes
from pathlib import Path
import hdf5plugin

lib = ctypes.CDLL(str(Path(hdf5plugin.PLUGINS_PATH) / "libh5blosc2.so"))
lib.blosc2_get_version_string.restype = ctypes.c_char_p
print("hdf5plugin Blosc2 runtime:", lib.blosc2_get_version_string().decode())
PY

# Install the HTJ2K plugin.  The default manifest selects kakadu first when it
# is usable, then openhtj2k.  No BLOSC2_HTJ2K_BACKEND variable is needed for
# this test.
git clone --recursive https://github.com/Blosc/blosc2_htj2k.git
python -m pip install -v --no-build-isolation --no-deps ./blosc2_htj2k

export HTJ2K_PACKAGE="$(python -c 'from pathlib import Path; import blosc2_htj2k; print(Path(blosc2_htj2k.__file__).resolve().parent)')"
export LD_LIBRARY_PATH="${BLOSC2_PACKAGE}/lib:${HTJ2K_PACKAGE}:${LD_LIBRARY_PATH:-}"

# Build a small stack, write an uncompressed HDF5 file and an HTJ2K-compressed
# HDF5 file, then check the decoded values.
python ./blosc2_htj2k/examples/quickstart.py

# Build and run the C++ HDF5 quickstart.  It writes a raw HDF5 stack and an
# HTJ2K-compressed HDF5 stack using the HDF5 Blosc2 filter.
h5c++ -std=c++17 ./blosc2_htj2k/examples/cpp_quickstart.cpp \
  -o ./cpp_htj2k_quickstart \
  -I"${BLOSC2_PACKAGE}/include" \
  -L"${BLOSC2_PACKAGE}/lib" \
  -Wl,-rpath,"${BLOSC2_PACKAGE}/lib" \
  -lblosc2

export HDF5_PLUGIN_PATH="$(python -c 'import hdf5plugin; print(hdf5plugin.PLUGINS_PATH)')"
./cpp_htj2k_quickstart

# Read the compressed HDF5 file using only the rebuilt hdf5plugin filter.
# c-blosc2 discovers libblosc2_htj2k.so through LD_LIBRARY_PATH, and backend
# choice comes from blosc2_htj2k_plugins.json.
env -u HDF5_PLUGIN_PATH -u BLOSC2_HTJ2K_BACKEND -u BLOSC2_HTJ2K_PLUGIN_PATH python - <<'PY'
import hdf5plugin
import h5py

with h5py.File("quickstart_output/htj2k_stack_blosc2_htj2k.h5", "r") as h5f:
    data = h5f["entry/data"][...]

print("read with normal hdf5plugin:", data.shape, data.dtype, int(data.sum()))
PY
```

</details>

After this, the same environment can be reused:

```bash
# Run these commands from the quickstart directory created above, e.g. /tmp/blosc2_htj2k_quickstart_...
source .venv/bin/activate
export BLOSC2_PACKAGE="$(python -c 'from pathlib import Path; import blosc2; print(Path(blosc2.__file__).resolve().parent)')"
export HTJ2K_PACKAGE="$(python -c 'from pathlib import Path; import blosc2_htj2k; print(Path(blosc2_htj2k.__file__).resolve().parent)')"
export LD_LIBRARY_PATH="${BLOSC2_PACKAGE}/lib:${HTJ2K_PACKAGE}:${LD_LIBRARY_PATH:-}"
```

Once PyPI `hdf5plugin` is rebuilt against a c-blosc2 release containing codec id
`40`, the local `hdf5plugin` build step can also collapse back to a normal
`hdf5plugin` dependency.

The quickstart script can also be run manually.  The basic lossless example is:

```bash
python ./blosc2_htj2k/examples/quickstart.py
```

This registers codec id `40`, lets the installed manifest choose the first
usable HTJ2K backend, creates a deterministic `uint16` stack, compresses it as
HTJ2K, decodes it, and checks exact equality with the input.  It also writes two
HDF5 files under `quickstart_output/`: an uncompressed reference stack and the
same stack compressed with the HDF5 Blosc2 filter using codec id `40`.  The
script prints the exact file paths, dataset path, compressed size, and error
metrics.  The OpenHTJ2K backend is built from the package submodule when no
external OpenHTJ2K installation is configured.

To force the OpenHTJ2K backend explicitly:

```bash
python ./blosc2_htj2k/examples/quickstart.py --backend openhtj2k
```

Run the float32 quantization example:

```bash
python ./blosc2_htj2k/examples/quickstart.py --float-mode uint16
```

This creates a deterministic `float32` stack, quantizes each chunk to `uint16`,
compresses the quantized integer image as HTJ2K, decodes it back to `float32`,
and checks that the measured error is within the expected quantization bound.
It writes the matching raw and compressed HDF5 files as well.

Run the lossy rate-control example:

```bash
python ./blosc2_htj2k/examples/quickstart.py --lossy --codec-meta 80
```

This creates the same deterministic `uint16` stack, asks the backend for lossy
compression through `codec_meta`, decodes it, and checks that the decoded image
remains within bounded error.  In this prototype, `codec_meta=80` is interpreted
as `8.0` in the backend rate mode.  A loose rate target can still decode
bit-identically when the chunk fits in the requested budget without losing
information.
The HDF5 pair is still written so the user can inspect the raw and compressed
stack files produced by the quick start.

Build and run the C++ quickstart:

```bash
export PKG_CONFIG_PATH="${BLOSC2_PACKAGE}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="${BLOSC2_PACKAGE}/lib:${HTJ2K_PACKAGE}:${LD_LIBRARY_PATH:-}"

h5c++ -std=c++17 ./blosc2_htj2k/examples/cpp_quickstart.cpp \
  -o ./cpp_htj2k_quickstart \
  -I"${BLOSC2_PACKAGE}/include" \
  -L"${BLOSC2_PACKAGE}/lib" \
  -Wl,-rpath,"${BLOSC2_PACKAGE}/lib" \
  -lblosc2

export HDF5_PLUGIN_PATH="$(python -c 'import hdf5plugin; print(hdf5plugin.PLUGINS_PATH)')"
./cpp_htj2k_quickstart
```

This program links to HDF5 and `libblosc2`.  It does not import Python and does
not link to `libblosc2_htj2k` explicitly.  The updated c-blosc2 registry knows
codec id `40`, `HDF5_PLUGIN_PATH` lets HDF5 discover `libh5blosc2`, and
`LD_LIBRARY_PATH` lets c-blosc2 discover `libblosc2_htj2k.so` at runtime.  The
program writes `quickstart_output/htj2k_stack_raw_cpp.h5` and
`quickstart_output/htj2k_stack_blosc2_htj2k_cpp.h5`.  The compressed file is
written with direct compressed chunks so the example is independent of HDF5
filter ABI details at write time, but the readback is the normal transparent
path: `H5Dread` asks HDF5 to load the Blosc2 filter, which then asks c-blosc2 to
load the HTJ2K codec from `LD_LIBRARY_PATH`.

Open the generated HDF5 files from a Python prompt.  In Python, importing
`hdf5plugin` is enough; do not pre-load the same plugin through
`HDF5_PLUGIN_PATH` before the import:

```python
>>> import h5py
>>> import hdf5plugin
>>> raw = h5py.File("quickstart_output/htj2k_stack_raw.h5", "r")
>>> compressed = h5py.File("quickstart_output/htj2k_stack_blosc2_htj2k.h5", "r")
>>> raw["entry/data"]
<HDF5 dataset "data": shape (4, 64, 96), type "<u2">
>>> compressed["entry/data"]
<HDF5 dataset "data": shape (4, 64, 96), type "<u2">
>>> stack = compressed["entry/data"][...]
>>> stack.shape, stack.dtype
((4, 64, 96), dtype('uint16'))
>>> (stack == raw["entry/data"][...]).all()
True
>>> raw.close(); compressed.close()
```

Minimal Python usage:

```python
import blosc2
import blosc2_htj2k
import numpy as np

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend="openhtj2k")

data = (np.arange(128 * 128, dtype=np.uint16).reshape(128, 128) % 4096)
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}

array = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
np.testing.assert_array_equal(array[...], data)
```

This is the minimal in-process API path: register the HTJ2K codec in the active
Blosc2 instance, choose a backend, compress a NumPy array into a Blosc2 NDArray,
and read it back.

Lossy mode uses `codec_meta`:

```python
cparams["codec_meta"] = 80  # interpreted as 8.0 in rate mode
```

When writing through the generic HDF5 Blosc2 filter, do not rely on
`hdf5plugin.Blosc2(cname="htj2k")`.  Keep `hdf5plugin` unchanged and pass the
numeric Blosc2 filter options instead:

```python
compression=hdf5plugin.BLOSC2_ID
compression_opts=blosc2_htj2k.hdf5_compression_opts(clevel=5)
```

This HDF5 option tuple only carries the codec id.  If you need `codec_meta`
for rate/quality mode, create Blosc2 cframes with that metadata and write them
as direct chunks.

## Float32 Quantization Mode

`float32` input is supported only when explicitly enabled.  The codec first
maps each float chunk to an integer image (`uint8`, `uint16`, or `uint32`),
compresses that integer image with the selected HTJ2K backend, and stores a
small private header with the scale metadata.  Decode restores `float32` bytes.

Integer `uint8`, `uint16`, and `uint32` inputs keep the normal path and are not
affected by this mode.

Recommended starting point:

```python
import blosc2_htj2k

blosc2_htj2k.register_codec()
blosc2_htj2k.configure(backend="openhtj2k", float_mode="uint16")
```

With optional scale guards:

```python
blosc2_htj2k.configure(
    backend="openhtj2k",
    float_mode="uint16",
    float_clamp_min=-1.0,
    float_clamp_max=1.0,
)
```

The clamp values do not force every chunk to use the full
`[float_clamp_min, float_clamp_max]` range.  They act as guards on the
per-chunk scale estimation.  For each chunk, the codec first computes the finite
raw minimum and maximum.  If clamp bounds are configured, the effective scale is
then limited by those bounds; conceptually:

```text
scale_min = max(raw_min, float_clamp_min)  # when a lower clamp is set
scale_max = min(raw_max, float_clamp_max)  # when an upper clamp is set
```

If the chunk range is already inside the clamp interval, the scale remains free
inside that interval and is still derived from the chunk data.  Only values that
fall outside the effective scale are saturated before quantization.  This is
intended to prevent anomalous values, for example divergent points produced by
an iterative algorithm, from expanding the quantization scale for the whole
chunk.

Environment-only configuration, useful for HDF5/C++/service deployments:

```bash
export BLOSC2_HTJ2K_FLOAT=uint16
export BLOSC2_HTJ2K_FLOAT_CLAMP_MIN=-1.0
export BLOSC2_HTJ2K_FLOAT_CLAMP_MAX=1.0
export BLOSC2_HTJ2K_FLOAT_NAN_POLICY=fail
```

Accepted `BLOSC2_HTJ2K_FLOAT` values are `off`, `8`, `16`, `32`, `uint8`,
`uint16`, and `uint32`.  `uint16` is the recommended default.  `uint32`
requires a backend that supports 32-bit samples, currently Kakadu.

For lossless inner JPEG2000 compression, the expected quantization error is:

```text
max_abs_error <= (scale_max - scale_min) / (2 * qmax) + float32_margin
```

where `qmax` is `255`, `65535`, or `4294967295`, and `scale_min` /
`scale_max` are the effective per-chunk scale values after the optional clamp
guards.  NaN and Inf values fail clearly in this v1 mode; masks are not
preserved.

Hands-on float examples:

```bash
python examples/quickstart.py --backend openhtj2k --float-mode uint16
python examples/quickstart.py --backend openhtj2k --float-mode uint8 --float-clamp-min -1 --float-clamp-max 1
```

## Installation

When published as a wheel:

```bash
pip install blosc2-htj2k -U
```

For local development:

```bash
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-20}"
python -m pip install -v --no-build-isolation --force-reinstall --no-deps .
```

OpenHTJ2K can be supplied as an installed library, as a source checkout, as the
`src/openhtj2k` submodule, or through the automatic CMake external build.

Using an existing OpenHTJ2K installation:

```bash
export OPENHTJ2K_ROOT=/path/to/openhtj2k/install
export OPENHTJ2K_INCLUDE_DIR="$OPENHTJ2K_ROOT/include/open_htj2k/interface"
export OPENHTJ2K_LIB_PATH="$OPENHTJ2K_ROOT/lib"
export LD_LIBRARY_PATH="$OPENHTJ2K_LIB_PATH:${LD_LIBRARY_PATH:-}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-20}"

CMAKE_ARGS="-DOPENHTJ2K_ROOT=$OPENHTJ2K_ROOT -DOPENHTJ2K_INCLUDE_DIR=$OPENHTJ2K_INCLUDE_DIR -DOPENHTJ2K_LIBRARY_DIR=$OPENHTJ2K_LIB_PATH" \
  python -m pip install -v --no-build-isolation --force-reinstall .
```

If Kakadu is available:

```bash
export KAKADU_ROOT=/path/to/kakadu
export KAKADU_INCLUDE_DIR="$KAKADU_ROOT/managed/all_includes"
export KAKADU_LIB_PATH="$KAKADU_ROOT/lib"
export LD_LIBRARY_PATH="$KAKADU_LIB_PATH:${LD_LIBRARY_PATH:-}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-20}"

CMAKE_ARGS="-DKAKADU_ROOT=$KAKADU_ROOT -DKAKADU_INCLUDE_DIR=$KAKADU_INCLUDE_DIR -DKAKADU_LIBRARY_DIR=$KAKADU_LIB_PATH" \
  python -m pip install -v --no-build-isolation --force-reinstall .
```

On macOS, use `DYLD_LIBRARY_PATH` instead of `LD_LIBRARY_PATH`.  On Windows,
add the directory containing dependent DLLs to `PATH` before starting Python or
the host application.

## Runtime Backend Configuration

The preferred model is explicit configuration before the first encode/decode or
HDF5 read/write:

```python
import blosc2_htj2k

blosc2_htj2k.configure(backend="openhtj2k")
```

All arguments are optional.  If `plugin_path` is omitted, the runtime searches
the default plugin root installed next to `libblosc2_htj2k`:

```text
<libblosc2_htj2k directory>/plugins
```

Named backend discovery resolves:

```text
${plugin_root}/htj2k/${backend}
```

For example:

```text
<libblosc2_htj2k directory>/plugins/htj2k/openhtj2k
```

The installed backend names are `openhtj2k`, `grok`, and optionally `kakadu`.
`grok` is installed by default but is selected only when requested explicitly or
when a custom manifest gives it priority.

Selection priority is:

1. Explicit API configuration: `blosc2_htj2k.configure()` or
   `blosc2_htj2k_configure()`.
2. Legacy direct-directory variable: `BLOSC2_HTJ2K_REPLACEMENT_DIR`.
3. Named backend variables: `BLOSC2_HTJ2K_PLUGIN_PATH` and
   `BLOSC2_HTJ2K_BACKEND`.
4. Installed manifest `blosc2_htj2k_plugins.json`.

Configuration is finalized on first codec use.  Later attempts to reconfigure
fail clearly.

Environment examples:

```bash
export BLOSC2_HTJ2K_BACKEND=openhtj2k
```

or:

```bash
export BLOSC2_HTJ2K_BACKEND=grok
```

or, for a custom plugin root:

```bash
export BLOSC2_HTJ2K_PLUGIN_PATH=/opt/blosc2_htj2k/plugins
export BLOSC2_HTJ2K_BACKEND=openhtj2k
```

Legacy direct-directory example:

```bash
export BLOSC2_HTJ2K_REPLACEMENT_DIR=/opt/blosc2_htj2k/plugins/htj2k/openhtj2k
```

Custom manifest example:

```json
{
  "version": 1,
  "plugin_path": "plugins",
  "priority": {
    "htj2k": ["kakadu", "openhtj2k", "grok"]
  }
}
```

## Diagnostics

From Python:

```python
import blosc2_htj2k

print(blosc2_htj2k.available_backends())
print(blosc2_htj2k.list_plugins())
print(blosc2_htj2k.diagnose())
print(blosc2_htj2k.selftest())
```

From the command line:

```bash
python -m blosc2_htj2k --list-plugins
python -m blosc2_htj2k --diagnose
python -m blosc2_htj2k --selftest
```

The diagnostic JSON reports plugin roots, manifest priority, selected backend,
resolved float configuration, loadability, ABI validity, dependent-library
loader errors, and relevant environment variables.

## Compression Parameters

For compatibility with the original `blosc2_grok` interface, the Python module
still exposes `set_params_defaults(...)` and the Grok-style parameter names.
The most commonly useful control for quick tests is `codec_meta`.

If `codec_meta` is zero or omitted, compression is lossless.  If it is non-zero,
rate mode is activated with:

```text
rate = codec_meta / 10.0
```

Example:

```python
cparams = {
    "codec": blosc2_htj2k.CODEC_ID,
    "codec_meta": 5 * 10,  # rate value 5.0
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
```

Only rates below `25.6` are supported through this one-byte notation.

## HDF5 And Loader Order

When reading or writing through HDF5, the HDF5 Blosc2 filter must also be
loaded.  In Python, this is normally done by `import hdf5plugin`.  In
non-Python programs, this is normally done through `HDF5_PLUGIN_PATH`.

For Python processes that use the Python API directly, importing
`blosc2_htj2k` loads the codec library with global visibility and checks that
the active c-blosc2 build knows the HTJ2K codec id:

```python
import hdf5plugin
import blosc2_htj2k
import h5py

blosc2_htj2k.configure()  # backend selected by blosc2_htj2k_plugins.json
```

Use `blosc2_htj2k.configure(backend="openhtj2k")` only when you want to force a
backend and bypass the manifest priority.

`LD_LIBRARY_PATH` only makes shared libraries discoverable.  For Python,
`import hdf5plugin` registers the HDF5 Blosc2 filter and initializes the HDF5
function table used by the filter.  Do not set `HDF5_PLUGIN_PATH` to the same
`hdf5plugin/plugins` directory before importing `hdf5plugin`, otherwise HDF5 can
load the filter first and bypass that initialization.  If this happens in an
interactive session, call `hdf5plugin.register("blosc2", force=True)`.

For transparent HDF5-only use, the HDF5 Blosc2 filter must also use a c-blosc2
runtime that already contains the HTJ2K registry entry.  If `hdf5plugin` embeds
an older c-blosc2 copy, the filter will fail before `libblosc2_htj2k.so` can be
used.
No new `hdf5plugin` codec name is required; writers can pass the numeric
Blosc2 filter options returned by
`blosc2_htj2k.hdf5_compression_opts()`.

Transparent HDF5 example with `h5py` and `hdf5plugin`:

```bash
export HTJ2K_PACKAGE=/path/to/site-packages/blosc2_htj2k
export BLOSC2_PACKAGE=/path/to/site-packages/blosc2

export LD_LIBRARY_PATH="${BLOSC2_PACKAGE}/lib:${HTJ2K_PACKAGE}:${LD_LIBRARY_PATH:-}"

python my_hdf5_reader_or_writer.py
```

No `BLOSC2_HTJ2K_BACKEND` variable is required for the default installed
layout: the backend is selected by `blosc2_htj2k_plugins.json`.

If the Kakadu backend is selected, add the Kakadu library directory as well:

```bash
export LD_LIBRARY_PATH=/path/to/kakadu/lib:${LD_LIBRARY_PATH}
export BLOSC2_HTJ2K_BACKEND=kakadu
```

For C/C++ applications, the explicit path is:

```c
#include "blosc2_htj2k_public.h"

blosc2_init();
blosc2_htj2k_register_codec();

blosc2_htj2k_runtime_config cfg = {0};
cfg.struct_size = sizeof(cfg);
/* Leave cfg.backend NULL to use blosc2_htj2k_plugins.json.
   Set cfg.backend = "openhtj2k", "grok", or "kakadu" to force a backend. */

if (blosc2_htj2k_configure(&cfg) != 0) {
    fprintf(stderr, "%s\n", blosc2_htj2k_last_error());
}
```

For unmodified C++ programs, HDF5 command-line tools, web servers, or web
clients that cannot call this API, use an HDF5 Blosc2 filter built against the
same HDF5 runtime and the updated c-blosc2.  Such a process discovers the HDF5
filter through `HDF5_PLUGIN_PATH` and discovers the HTJ2K codec through
`LD_LIBRARY_PATH`:

```bash
export HDF5_PLUGIN_PATH=/path/to/hdf5/plugins
export LD_LIBRARY_PATH=/path/to/blosc2/lib:/path/to/blosc2_htj2k:${LD_LIBRARY_PATH:-}
```

As a temporary local workaround for an already-built static filter, preloading
the updated `libblosc2` can force symbol interposition:

```bash
export LD_PRELOAD=/path/to/blosc2/lib/libblosc2.so${LD_PRELOAD:+:${LD_PRELOAD}}
```

The expected registry entries are:

```text
j2k   -> 39
htj2k -> 40
```

This is a deployment bridge for loader-order issues in HDF5-only or service
runtimes.  It assumes a c-blosc2 build that includes the JPEG2000 ids in the
built-in registry.  Files written with the old pre-registry temporary ids need
the corresponding historical branch, and older c-blosc2 builds that do not know
id `40` must be updated; the public Blosc2 user-codec API cannot add global ids
below the user range.

## Current Tests

The current tests cover:

- manifest and plugin listing;
- codec registry check with id `40`;
- lossless HTJ2K roundtrip;
- explicit Grok HTJ2K backend roundtrip;
- lossy HTJ2K roundtrip through every available HTJ2K backend plugin;
- optional Kakadu `uint32` lossless roundtrip when Kakadu is installed;
- command-line diagnostics.

## Limitations

- Codec id `40` requires a c-blosc2 build that knows the HTJ2K registry entry.
  The bootstrap helps with loader order, but it cannot add a new global id to an
  older c-blosc2 build through the public user-codec API.
- Kakadu is optional and not redistributable.
- OpenHTJ2K is the redistributable open-source backend, currently based on the
  `uint16` C++ API available in OpenHTJ2K `v0.4.0` and newer.
- Grok is installed as an explicit HTJ2K backend, but its HTJ2K rate-control
  path is not used here; select OpenHTJ2K or Kakadu for lossy/rate-target
  tests.
- OpenHTJ2K currently handles the `uint16` path; the optional Kakadu backend
  also supports `uint32`.
- For Kakadu `uint32`, the default wavelet decomposition is capped at
  `Clevels=3` for robust small-chunk operation. Advanced users can override it
  with `BLOSC2_HTJ2K_CLEVELS` or `BLOSC2_HTJ2K_KAKADU_PARAMS`.
- The minimum practical image payload is around 256 bytes.

## Current Integration Status

1. This repository now lives under the Blosc organization as the candidate
   `blosc2_htj2k` plugin.
2. The c-blosc2 registry entry for `BLOSC_CODEC_HTJ2K = 40` has been merged
   upstream.
3. The bundled OpenHTJ2K backend now points to the official OpenHTJ2K `v0.4.0`
   release, whose API contains the changes previously tracked through PR #190.
4. python-blosc2 `4.4.3` exposes `blosc2.Codec.HTJ2K` and ships a c-blosc2
   runtime that resolves codec id `40`.
5. The README demo quickstart and CI workflows now use the released
   `blosc2>=4.4.3` dependency instead of a temporary python-blosc2 branch.
6. Once hdf5plugin wheels are rebuilt against a c-blosc2 release with codec id
   `40`, the README demo can drop the local hdf5plugin source-build workaround.
7. Keep validating Python, C/C++, HDF5, and service-runtime usage on Linux,
   macOS, and Windows wheels.
8. Keep the bootstrap only as a loader-order helper for HDF5-only deployments.
9. Keep the backend ABI independent from the Blosc2-facing codec.
10. Keep OpenHTJ2K as the preferred redistributable backend, Grok as an
   always-installed fallback, and Kakadu as an optional external backend.

## More Examples

See the [examples](examples/) directory.

## Thanks

Thanks to Francesc Alted for the guidance on the Blosc2 plugin direction,
including the suggestion to split J2K and HTJ2K into separate codecs.  This work
builds on the original `blosc2_grok` plugin and on the Grok JPEG2000 library.
