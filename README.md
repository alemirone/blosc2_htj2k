# blosc2_htj2k

`blosc2_htj2k` is an experimental Blosc2 codec plugin for High Throughput
JPEG2000 / HTJ2K codestreams.

It is a standalone split-out version of the HTJ2K part of the previous
`blosc2_grok` runtime-backend prototype.  The goal is to keep the Blosc2 codec
small and backend-agnostic: the Blosc2 codec is called `htj2k`, and the actual
HTJ2K implementation is selected through backend plugins installed inside the
package.

During the transition before official c-blosc2 codec ids are assigned, this
package registers a temporary dynamic codec:

```text
codec name:     htj2k
temporary id:   161
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

Clone and install the plugin in a fresh environment:

```bash
git clone https://github.com/alemirone/blosc2_htj2k.git
cd blosc2_htj2k

python -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -v --no-build-isolation .
```

Run the quick example:

```bash
python examples/quickstart.py
```

The same example can force a backend:

```bash
python examples/quickstart.py --backend openhtj2k
```

and can exercise lossy rate mode:

```bash
python examples/quickstart.py --backend openhtj2k --lossy --codec-meta 80
```

The script creates a deterministic `uint16` image, compresses it with codec id
`161`, decodes it, and prints compression and error metrics.  In lossless mode
it checks exact equality.  In lossy mode it checks that the decoded image is not
bit-identical but remains within bounded error.

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

CMAKE_ARGS="-DOPENHTJ2K_ROOT=$OPENHTJ2K_ROOT -DOPENHTJ2K_INCLUDE_DIR=$OPENHTJ2K_INCLUDE_DIR -DOPENHTJ2K_LIBRARY_DIR=$OPENHTJ2K_LIB_PATH" \
  python -m pip install -v --no-build-isolation --force-reinstall .
```

If Kakadu is available:

```bash
export KAKADU_ROOT=/path/to/kakadu
export KAKADU_INCLUDE_DIR="$KAKADU_ROOT/managed/all_includes"
export KAKADU_LIB_PATH="$KAKADU_ROOT/lib"
export LD_LIBRARY_PATH="$KAKADU_LIB_PATH:${LD_LIBRARY_PATH:-}"

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
discoverable:

```bash
export HDF5_PLUGIN_PATH="$(python -c 'import hdf5plugin; print(hdf5plugin.PLUGIN_PATH)')"
```

For Python processes that use the Python API directly, importing
`blosc2_htj2k` can register the temporary codec id and load the codec library
with global visibility:

```python
import hdf5plugin
import blosc2_htj2k
import h5py

blosc2_htj2k.configure(backend="openhtj2k")
```

This is enough for Python code that calls the `blosc2_htj2k` registration path
before compression/decompression.  It is not always enough for a fully
transparent HDF5-only path, because the HDF5 Blosc2 filter may use a separate
Blosc2 library instance.  In that case the filter can find Blosc2 and the HDF5
plugin, but codec id `161` is still absent from the Blosc2 codec registry.

`LD_LIBRARY_PATH` only makes shared libraries discoverable.  `HDF5_PLUGIN_PATH`
only makes the HDF5 Blosc2 filter discoverable.  Neither one registers the
temporary codec id in Blosc2.  During the temporary-id phase, `LD_PRELOAD` is
the robust way to register codec id `161` before HDF5 starts using Blosc2.

Transparent HDF5 example with `h5py` and `hdf5plugin`:

```bash
export HTJ2K_PACKAGE=/path/to/site-packages/blosc2_htj2k
export BLOSC2_PACKAGE=/path/to/site-packages/blosc2

export HDF5_PLUGIN_PATH=/path/to/hdf5plugin/plugins
export LD_LIBRARY_PATH="${HTJ2K_PACKAGE}:${BLOSC2_PACKAGE}/lib:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="${HTJ2K_PACKAGE}/libblosc2_jpeg2000_bootstrap.so${LD_PRELOAD:+:${LD_PRELOAD}}"

export BLOSC2_HTJ2K_BACKEND=openhtj2k

python my_hdf5_reader_or_writer.py
```

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
cfg.backend = "openhtj2k";

if (blosc2_htj2k_configure(&cfg) != 0) {
    fprintf(stderr, "%s\n", blosc2_htj2k_last_error());
}
```

For unmodified C++ programs, HDF5 command-line tools, web servers, or web
clients that cannot call this API, preload the bootstrap library:

```bash
export HDF5_PLUGIN_PATH=/path/to/hdf5/plugins
export LD_LIBRARY_PATH=/path/to/blosc2/lib:/path/to/blosc2_htj2k:/path/to/openhtj2k/lib:${LD_LIBRARY_PATH:-}
export LD_PRELOAD=/path/to/blosc2_htj2k/libblosc2_jpeg2000_bootstrap.so${LD_PRELOAD:+:${LD_PRELOAD}}
```

The bootstrap calls `blosc2_init()` and registers both temporary JPEG2000 ids
used during this prototype:

```text
j2k   -> 160
htj2k -> 161
```

This is only a deployment bridge for the temporary-id phase.  With official
c-blosc2 codec ids, this should become simpler.  Until then, files written with
codec id `161` need either explicit registration in the application or this
bootstrap preload mechanism before they can be read through a transparent HDF5
path.

## Current Tests

The current tests cover:

- manifest and plugin listing;
- codec registration with temporary id `161`;
- lossless HTJ2K roundtrip;
- lossy HTJ2K roundtrip through every available HTJ2K backend plugin;
- optional Kakadu `uint32` lossless roundtrip when Kakadu is installed;
- command-line diagnostics.

## Limitations

- Codec id `161` is temporary.
- Files written with id `161` require the same temporary registration mechanism
  until an official c-blosc2 id exists.
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
3. Ask the c-blosc2 maintainers for an official codec id for `htj2k`.
4. Replace temporary id `161` with the official id.
5. Keep the backend ABI independent from the Blosc2-facing codec.
6. Keep OpenHTJ2K as the redistributable backend and Kakadu as an optional
   external backend.

## More Examples

See the [examples](examples/) directory.

## Thanks

Thanks to Francesc Alted for the guidance on the Blosc2 plugin direction,
including the suggestion to split J2K and HTJ2K into separate codecs.  This work
builds on the original `blosc2_grok` plugin and on the Grok JPEG2000 library.
