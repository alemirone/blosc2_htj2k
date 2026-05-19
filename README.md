# blosc2_htj2k

`blosc2_htj2k` is an experimental Blosc2 codec plugin for High Throughput
JPEG2000 / HTJ2K codestreams.

It is a standalone split-out version of the HTJ2K part of the previous
`blosc2_grok` runtime-backend prototype.  The goal is to keep the Blosc2 codec
small and backend-agnostic: the Blosc2 codec is called `htj2k`, and the actual
HTJ2K implementation is selected through backend plugins installed inside the
package.

The package uses the HTJ2K codec id prepared for the c-blosc2 registry:

```text
codec name:     htj2k
codec id:       40
library:        libblosc2_htj2k.so
Python package: blosc2_htj2k
```

The codec is plugin-only.  There is no Grok/native HTJ2K fallback in this
package.  Backend selection comes from explicit configuration, environment
variables, or the installed manifest.

The default manifest is:

```json
{
  "htj2k": ["kakadu", "openhtj2k"]
}
```

This means: use Kakadu when it is installed and loadable, otherwise use the
redistributable OpenHTJ2K backend.

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
      kakadu/
        libblosc2_kakadu_htj2k_backend.so
```

Backend capabilities:

| Backend | Built when | HTJ2K | `uint8` | `uint16` | `uint32` | Redistributable |
| --- | --- | --- | --- | --- | --- | --- |
| `plugins/htj2k/openhtj2k` | OpenHTJ2K PR190-style API found or built | yes | yes | yes | no | yes |
| `plugins/htj2k/kakadu` | Kakadu found | yes | yes | yes | yes | no |

Kakadu is optional and is not redistributed by this project.

## Hands-On Quick Start

For now this plugin depends on the experimental `c-blosc2` and `python-blosc2`
branches that define and expose the proposed J2K/HTJ2K codec IDs.  The quick
start is therefore a full-stack install.

The important point is that `hdf5plugin` must be built against the same
experimental `libblosc2` runtime.  The standard `hdf5plugin` wheel embeds its
own c-blosc2 copy, so changing `LD_LIBRARY_PATH` is not enough for that wheel to
know codec id `40`.  The commands below rebuild only the HDF5 Blosc2 filter
from `hdf5plugin` with `HDF5PLUGIN_SYSTEM_LIBRARIES=blosc2`, which makes that
filter use the experimental `libblosc2` installed by `python-blosc2`.

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

The long block below bootstraps the current experimental stack from source.  It
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
python -m pip install scikit-build-core cython numpy pkgconfig py-cpuinfo h5py
export CMAKE_BUILD_PARALLEL_LEVEL=20

# Install python-blosc2 from the experimental branch.  This branch bundles the
# matching experimental c-blosc2 runtime in site-packages/blosc2/lib.
git clone https://github.com/alemirone/python-blosc2.git
git -C python-blosc2 checkout add_j2k_htj2k_custom_codecs
python -m pip install -v --no-build-isolation ./python-blosc2

export BLOSC2_PACKAGE="$(python - <<'PY'
from pathlib import Path
import blosc2
print(Path(blosc2.__file__).resolve().parent)
PY
)"

# Make pkg-config point to the libblosc2 shipped by this venv installation.
# This keeps the following hdf5plugin build on the same experimental c-blosc2.
cat > "${BLOSC2_PACKAGE}/lib/pkgconfig/blosc2.pc" <<EOF
libdir=${BLOSC2_PACKAGE}/lib
includedir=${BLOSC2_PACKAGE}/include

Name: blosc2
Description: A blocking, shuffling and lossless compression library
URL: https://blosc.org/
Version: 3.0.4.dev
Libs: -L\${libdir} -Wl,-rpath,\${libdir} -l:libblosc2.so.8
Cflags: -I\${includedir}
EOF

export PKG_CONFIG_PATH="${BLOSC2_PACKAGE}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="${BLOSC2_PACKAGE}/lib:${LD_LIBRARY_PATH:-}"

python - <<'PY'
import blosc2
print("blosc2:", blosc2.__version__)
print("J2K codec:", blosc2.Codec.J2K)
print("HTJ2K codec:", blosc2.Codec.HTJ2K)
PY

# Rebuild hdf5plugin so libh5blosc2 uses the experimental libblosc2 above
# instead of an embedded c-blosc2 copy.
git clone https://github.com/silx-kit/hdf5plugin.git
CFLAGS="${CFLAGS:-} -DBLOSC2_MAX_DIM=B2ND_MAX_DIM" \
HDF5PLUGIN_SYSTEM_LIBRARIES=blosc2 \
HDF5PLUGIN_STRIP=blosc,bshuf,bzip2,fcidecomp,lz4,sperr,sz,sz3,zfp,zstd \
  python -m pip install -v --no-build-isolation --no-deps ./hdf5plugin

# Install the HTJ2K plugin.  The default manifest selects kakadu first when it
# is usable, then openhtj2k.  No BLOSC2_HTJ2K_BACKEND variable is needed for
# this test.
git clone --recursive https://github.com/alemirone/blosc2_htj2k.git
python -m pip install -v --no-build-isolation --no-deps ./blosc2_htj2k

export HTJ2K_PACKAGE="$(python - <<'PY'
from pathlib import Path
import blosc2_htj2k
print(Path(blosc2_htj2k.__file__).resolve().parent)
PY
)"
export LD_LIBRARY_PATH="${BLOSC2_PACKAGE}/lib:${HTJ2K_PACKAGE}:${LD_LIBRARY_PATH:-}"

# Build a small stack, write an uncompressed HDF5 file and an HTJ2K-compressed
# HDF5 file, then check the decoded values.
python ./blosc2_htj2k/examples/quickstart.py

# Read the compressed HDF5 file in the simple Python deployment mode:
# hdf5plugin only.  Backend choice comes from blosc2_htj2k_plugins.json.
env -u HDF5_PLUGIN_PATH -u BLOSC2_HTJ2K_BACKEND -u BLOSC2_HTJ2K_PLUGIN_PATH python - <<'PY'
import hdf5plugin
import h5py

with h5py.File("quickstart_output/htj2k_stack_blosc2_htj2k.h5", "r") as h5f:
    data = h5f["entry/data"][...]

print("read with hdf5plugin only:", data.shape, data.dtype, int(data.sum()))
PY
```

</details>

After this, the same environment can be reused:

```bash
cd /tmp/blosc2_htj2k_quickstart_XXXXXXXX
source .venv/bin/activate
export BLOSC2_PACKAGE="$(python - <<'PY'
from pathlib import Path
import blosc2
print(Path(blosc2.__file__).resolve().parent)
PY
)"
export HTJ2K_PACKAGE="$(python - <<'PY'
from pathlib import Path
import blosc2_htj2k
print(Path(blosc2_htj2k.__file__).resolve().parent)
PY
)"
export LD_LIBRARY_PATH="${BLOSC2_PACKAGE}/lib:${HTJ2K_PACKAGE}:${LD_LIBRARY_PATH:-}"
```

Once the codec IDs are merged and released upstream, the `python-blosc2`
clone/build step collapses back to a normal released `blosc2` dependency.  Once
`hdf5plugin` is released against a c-blosc2 containing these IDs, the
`hdf5plugin` rebuild step also collapses back to `python -m pip install
hdf5plugin`.

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
is not bit-identical but remains within bounded error.  In this prototype,
`codec_meta=80` is interpreted as `8.0` in the backend rate mode.
The HDF5 pair is still written so the user can inspect the raw and compressed
stack files produced by the quick start.

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

or, for a custom plugin root:

```bash
export BLOSC2_HTJ2K_PLUGIN_PATH=/opt/blosc2_htj2k/plugins
export BLOSC2_HTJ2K_BACKEND=openhtj2k
```

Legacy direct-directory example:

```bash
export BLOSC2_HTJ2K_REPLACEMENT_DIR=/opt/blosc2_htj2k/plugins/htj2k/openhtj2k
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
   Set cfg.backend = "openhtj2k" or "kakadu" to force a backend. */

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

If the HDF5 filter was built with an embedded old c-blosc2 copy, rebuild it with:

```bash
export HDF5PLUGIN_SYSTEM_LIBRARIES=blosc2
export HDF5PLUGIN_STRIP=blosc,bshuf,bzip2,fcidecomp,lz4,sperr,sz,sz3,zfp,zstd
export CFLAGS="${CFLAGS:-} -DBLOSC2_MAX_DIM=B2ND_MAX_DIM"
export PKG_CONFIG_PATH=/path/to/blosc2/lib/pkgconfig:${PKG_CONFIG_PATH:-}
export LD_LIBRARY_PATH=/path/to/blosc2/lib:${LD_LIBRARY_PATH:-}
python -m pip install -v --no-build-isolation --force-reinstall --no-deps /path/to/hdf5plugin
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
built-in registry.  Older temporary-id files need the previous temporary-id
branch, and older c-blosc2 builds that do not know id `40` must be updated; the
public Blosc2 user-codec API cannot add global ids below the user range.

## Current Tests

The current tests cover:

- manifest and plugin listing;
- codec registry check with id `40`;
- lossless HTJ2K roundtrip;
- lossy HTJ2K roundtrip through every available HTJ2K backend plugin;
- optional Kakadu `uint32` lossless roundtrip when Kakadu is installed;
- command-line diagnostics.

## Limitations

- Codec id `40` requires a c-blosc2 build that knows the HTJ2K registry entry.
  The bootstrap helps with loader order, but it cannot add a new global id to an
  older c-blosc2 build through the public user-codec API.
- Kakadu is optional and not redistributable.
- OpenHTJ2K is the redistributable open-source backend, currently based on the
  PR190-style `uint16` API.
- OpenHTJ2K currently handles the PR190-style `uint16` path; the optional
  Kakadu backend also supports `uint32`.
- For Kakadu `uint32`, the default wavelet decomposition is capped at
  `Clevels=3` for robust small-chunk operation. Advanced users can override it
  with `BLOSC2_HTJ2K_CLEVELS` or `BLOSC2_HTJ2K_KAKADU_PARAMS`.
- The minimum practical image payload is around 256 bytes.

## Next Steps Toward An Official Plugin

1. Publish this standalone repository as the candidate `blosc2_htj2k` plugin.
2. Validate Python, C/C++, HDF5, and service-runtime usage.
3. Merge the c-blosc2 registry PR that adds `BLOSC_CODEC_HTJ2K = 40`.
4. Keep the bootstrap only as a loader-order helper for HDF5-only deployments.
5. Keep the backend ABI independent from the Blosc2-facing codec.
6. Keep OpenHTJ2K as the redistributable backend and Kakadu as an optional
   external backend.

## More Examples

See the [examples](examples/) directory.

## Thanks

Thanks to Francesc Alted for the guidance on the Blosc2 plugin direction,
including the suggestion to split J2K and HTJ2K into separate codecs.  This work
builds on the original `blosc2_grok` plugin and on the Grok JPEG2000 library.
