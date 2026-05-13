# Blosc2 grok

A plugin of the excellent [grok library](https://github.com/GrokImageCompression/grok) for Blosc2.  grok is a JPEG2000 codec, and with this plugin, you can use it as yet another codec in applications using Blosc2.  See an example of use at: https://github.com/Blosc/blosc2_grok/blob/main/examples/params.py

## Installation

For using `blosc2_grok` you will first have to install its wheel:

```shell
pip install blosc2-grok -U
```

## Usage

```python
import blosc2
import numpy as np
import blosc2_grok
from PIL import Image

# Set the params for the grok codec
kwargs = {}
kwargs['cod_format'] = blosc2_grok.GrkFileFmt.GRK_FMT_JP2
kwargs['quality_mode'] = "dB"
kwargs['quality_layers'] = np.array([5], dtype=np.float64)
blosc2_grok.set_params_defaults(**kwargs)

# Define the compression and decompression parameters for Blosc2.
# Disable the filters and do not split blocks (these won't work with grok).
cparams = {
    'codec': blosc2.Codec.GROK,
    'filters': [],
    'splitmode': blosc2.SplitMode.NEVER_SPLIT,
}

# Read the image
im = Image.open("examples/kodim23.png")
# Convert the image to a numpy array
np_array = np.asarray(im)

# Transform the numpy array to a blosc2 array. This is where compression happens.
bl_array = blosc2.asarray(
    np_array,
    chunks=np_array.shape,
    blocks=np_array.shape,
    cparams=cparams,
    urlpath="examples/kodim23.b2nd",
    mode="w",
)

# Print information about the array, see the compression ratio (cratio)
print(bl_array.info)
```

## Parameters for compression

The following parameters are available for compression for grok, with their defaults.  Most of them are named after the ones in the [Pillow library](https://pillow.readthedocs.io/en/stable/handbook/image-file-formats.html#jpeg-2000-saving) and have the same meaning.  The ones that are not in Pillow are marked with a `*` and you can get more information about them in the [grok documentation](https://github.com/GrokImageCompression/grok/wiki/3.-grk_compress), or by following the provided links.  For those marked with a ``**``, you can get more information in the [grok.h header](https://github.com/GrokImageCompression/grok/blob/a84ac2592e581405a976a00cf9e6f03cab7e2481/src/lib/core/grok.h#L975
).

    'tile_size': (0, 0),
    'tile_offset': (0, 0),
    'quality_mode': None,
    'quality_layers': np.zeros(0, dtype=np.float64),
    'progression': "LRCP",
    'num_resolutions': 6,
    'codeblock_size': (64, 64),
    'irreversible': False,
    'precinct_size': (0, 0),
    'offset': (0, 0),
    'mct': 0,
    * 'numgbits': 2,  # Equivalent to -N, -guard_bits
    * 'roi_compno': -1,  # Together with 'roi_shift' it is equivalent to -R, -ROI
    * 'roi_shift': 0,
    * 'decod_format': GrkFileFmt.GRK_FMT_UNK,
    * 'cod_format': GrkFileFmt.GRK_FMT_UNK,
    * 'rsiz': GrkProfile.GRK_PROFILE_NONE,  # Equivalent to -Z, -rsiz
    * 'framerate': 0,
    * 'apply_icc_': False,  # Equivalent to -f, -apply_icc
    * 'rateControlAlgorithm': GrkRateControl.BISECT,
    * 'num_threads': 0,
    * 'deviceId': 0,  # Equivalent to -G, -device_id
    * 'duration': 0,  # Equivalent to -J, -duration
    * 'repeats': 1,  # Equivalent to -e, -repetitions
    * 'mode': GrkMode.DEFAULT,  # Equivalent to -M, -mode
    * 'verbose': False,  # Equivalent to -v, -verbose
    ** 'enableTilePartGeneration': False,  # See header of grok.h above
    ** 'max_cs_size': 0,  # See header of grok.h above
    ** 'max_comp_size': 0,  # See header of grok.h above

*Note: * when using the `blosc2_grok` plugin from C, the structure used
for setting the parameters uses the `grok` parameters names. You can see an example
in https://github.com/Blosc/leaps-examples/blob/main/c-compression/compress-tomo.c#L110 .

### codec_meta as rates quality mode

As a simpler way to activate the rates quality mode, if you set the `codec_meta` from the `cparams` to an
integer different from 0, the rates quality mode will be activated with a rate value equal to `codec_meta` / 10. If 
`cod_format` is not specified, the default will be used. The `codec_meta` has priority to the `rates` param set with the 
`blosc2_grok.set_params_defaults()`. Please note that only rates < 25.6 are supported with this notation.
```python
import blosc2


cparams = {
    'codec': blosc2.Codec.GROK,
    'codec_meta': 5 * 10,  # cratio will be 5
    'filters': [],
    'splitmode': blosc2.SplitMode.NEVER_SPLIT,
}
```

## Runtime backend configuration

`blosc2_grok` can route J2K and HTJ2K codestreams through family-specific
runtime backends.  The preferred model is explicit configuration before the
first encode/decode operation: Python users call `blosc2_grok.configure()`, and
C/C++ hosts call `blosc2_grok_configure()`.  Environment variables are still
supported for command-line tools, HDF5-only deployments, and backwards
compatibility.

Regular J2K has a built-in Grok path and therefore works without any runtime
plugin.  HTJ2K does not use the native Grok path in this package; HTJ2K
encode/decode requires an HTJ2K backend such as OpenHTJ2K or Kakadu.

Runtime plugins are split by codestream family:

* J2K plugins export `J2K_CODEC_PLUGIN`, defined in
  `src/plugin/j2k_codec_api.h`.
* HTJ2K plugins export `HTJ2K_CODEC_PLUGIN`, defined in
  `src/plugin/htj2k_codec_api.h`.

Named backend discovery resolves plugins as:

```text
${plugin_root}/${family}/${backend}
```

For example, `plugin_root=/opt/blosc2_grok/plugins`,
`family=htj2k`, and `backend=openhtj2k` selects:

```text
/opt/blosc2_grok/plugins/htj2k/openhtj2k
```

### Python configuration

Use `configure()` before the first Blosc2 encode/decode or HDF5 read/write:

```python
import blosc2_grok

blosc2_grok.configure(
    plugin_path="/opt/blosc2_grok/plugins",
    j2k_backend="grok",
    htj2k_backend="openhtj2k",
)
```

All arguments are optional.  A typical HTJ2K-only configuration can leave J2K
untouched:

```python
import blosc2_grok

blosc2_grok.configure(
    plugin_path="/opt/blosc2_grok/plugins",
    htj2k_backend="openhtj2k",
)
```

The runtime can be inspected from Python:

```python
import blosc2_grok

print(blosc2_grok.available_backends())
print(blosc2_grok.list_plugins())
print(blosc2_grok.diagnose())
print(blosc2_grok.selftest())
```

The same diagnostics are available from the command line:

```bash
python -m blosc2_grok --list-plugins
python -m blosc2_grok --diagnose
python -m blosc2_grok --selftest
```

### C/C++ configuration

C/C++ applications that link or explicitly load `libblosc2_grok` should
configure the runtime before opening HDF5 files or using Blosc2 data that may
need the codec:

```c
#include "blosc2_grok_public.h"

blosc2_grok_runtime_config cfg = {0};
cfg.struct_size = sizeof(cfg);
cfg.plugin_path = "/opt/blosc2_grok/plugins";
cfg.j2k_backend = "grok";
cfg.htj2k_backend = "openhtj2k";

if (blosc2_grok_configure(&cfg) != 0) {
    fprintf(stderr, "%s\n", blosc2_grok_last_error());
}
```

`blosc2_grok_list_plugins()` and `blosc2_grok_diagnose()` return JSON text into
a caller-provided buffer.  Passing `NULL, 0` returns the required byte count.

### Environment-variable configuration

Environment variables remain useful when the host application cannot call the
configuration API.  If no explicit API call has been made, backend selection is:

1. Legacy direct-directory variables:
   `BLOSC2_GROK_REPLACEMENT_DIR` for J2K and
   `BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR` for HTJ2K.
2. Named backend variables:
   `BLOSC2_GROK_PLUGIN_PATH`, `BLOSC2_GROK_J2K_BACKEND`, and
   `BLOSC2_GROK_HTJ2K_BACKEND`.
3. Defaults: native Grok for J2K, and no backend for HTJ2K.

An explicit API call has priority over all backend-selection environment
variables.  Configuration is finalized on first codec use; later calls to
`configure()` or `blosc2_grok_configure()` fail with a clear error.

Named backend example:

```bash
export BLOSC2_GROK_PLUGIN_PATH="/opt/blosc2_grok/plugins"
export BLOSC2_GROK_J2K_BACKEND="grok"
export BLOSC2_GROK_HTJ2K_BACKEND="openhtj2k"
```

Legacy direct-directory examples:

```bash
export BLOSC2_GROK_REPLACEMENT_DIR="/opt/blosc2_grok/plugins/j2k/grok"
export BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR="/opt/blosc2_grok/plugins/htj2k/openhtj2k"
```

### Available plugins

The source tree always builds and installs a small Grok J2K replacement backend
in `blosc2_grok/plugins/j2k/grok`.  This backend is intentionally equivalent to
the native J2K path and exists to exercise the generic plugin mechanism.

If OpenHTJ2K headers and libraries are available at build time, CMake builds
`blosc2_grok/plugins/htj2k/openhtj2k`.  OpenHTJ2K discovery can be configured
with `OPENHTJ2K_ROOT`, `OPENHTJ2K_INCLUDE_DIR` and `OPENHTJ2K_LIBRARY_DIR` or
`OPENHTJ2K_LIB_PATH`.  The backend is enabled only when a real CMake
compile/link probe validates the PR #190-style `uint16` API.

If Kakadu headers and libraries are available at build time, CMake builds two
physical Kakadu plugins sharing the same internal implementation:
`blosc2_grok/plugins/j2k/kakadu` and `blosc2_grok/plugins/htj2k/kakadu`.
Kakadu discovery can be configured with `KAKADU_ROOT`, `KAKADU_INCLUDE_DIR` and
`KAKADU_LIBRARY_DIR` or `KAKADU_LIB_PATH`.  Kakadu is optional and not
redistributed by this project.

Backend capabilities:

| Backend | Built when | J2K | HTJ2K | `uint8` | `uint16` |
| --- | --- | --- | --- | --- | --- |
| native Grok | always | yes | no | yes | J2K only |
| `plugins/j2k/grok` | always | yes | no | yes | J2K only |
| `plugins/htj2k/openhtj2k` | OpenHTJ2K PR #190 API found | no | yes | yes | yes |
| `plugins/j2k/kakadu` | Kakadu found | yes | no | yes | yes |
| `plugins/htj2k/kakadu` | Kakadu found | no | yes | yes | yes |

### Building optional backends

OpenHTJ2K example:

```bash
export OPENHTJ2K_ROOT=/path/to/openhtj2k/install
export OPENHTJ2K_INCLUDE_DIR="$OPENHTJ2K_ROOT/include/open_htj2k/interface"
export OPENHTJ2K_LIB_PATH="$OPENHTJ2K_ROOT/lib"
export LD_LIBRARY_PATH="$OPENHTJ2K_LIB_PATH:${LD_LIBRARY_PATH:-}"

CMAKE_ARGS="-DOPENHTJ2K_ROOT=$OPENHTJ2K_ROOT -DOPENHTJ2K_INCLUDE_DIR=$OPENHTJ2K_INCLUDE_DIR -DOPENHTJ2K_LIBRARY_DIR=$OPENHTJ2K_LIB_PATH" \
  pip install -v --no-build-isolation --force-reinstall .
```

Kakadu example, when local Kakadu libraries are available:

```bash
export KAKADU_ROOT=/path/to/kakadu
export KAKADU_INCLUDE_DIR="$KAKADU_ROOT/managed/all_includes"
export KAKADU_LIB_PATH="$KAKADU_ROOT/lib"
export LD_LIBRARY_PATH="$KAKADU_LIB_PATH:${LD_LIBRARY_PATH:-}"

CMAKE_ARGS="-DKAKADU_ROOT=$KAKADU_ROOT -DKAKADU_INCLUDE_DIR=$KAKADU_INCLUDE_DIR -DKAKADU_LIBRARY_DIR=$KAKADU_LIB_PATH" \
  pip install -v --no-build-isolation --force-reinstall .
```

On macOS, use `DYLD_LIBRARY_PATH` instead of `LD_LIBRARY_PATH`.  On Windows,
add the directory containing dependent DLLs to `PATH` before starting Python or
the host application.

### HDF5 and loader order

When reading or writing through HDF5, the HDF5 Blosc2 filter must also be
discoverable:

```bash
export HDF5_PLUGIN_PATH="$(python -c 'import hdf5plugin; print(hdf5plugin.PLUGIN_PATH)')"
```

For Python processes, importing `blosc2_grok` before the first HDF5 read/write
loads the native codec library with global visibility:

```python
import blosc2_grok
import hdf5plugin
import h5py
```

For C/C++ applications, the preferred approach is to link or load
`libblosc2_grok` and call `blosc2_grok_configure()` before HDF5 starts reading
compressed datasets.  `LD_PRELOAD` remains useful for applications or command
line tools where no explicit initialization hook is available:

```bash
export BLOSC2_GROK_LIBRARY="$(python -c 'import blosc2_grok; print(blosc2_grok.libpath)')"
export LD_PRELOAD="${BLOSC2_GROK_LIBRARY}${LD_PRELOAD:+:$LD_PRELOAD}"
```

In deployments without Python, set `BLOSC2_GROK_LIBRARY` directly to the
installed `libblosc2_grok.so` path.  The preload step ensures that HDF5's
Blosc2 filter and external codec resolution see the same already-loaded codec
library.

If the dynamic loader reports that a shared object or DLL cannot be opened, the
backend was found but one of its dependent libraries was not.  In practice,
this usually means a backend library directory is missing from
`LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH` or `PATH`, depending on the platform.

## Notes

When using `blosc2_grok`, there are some restrictions that you have
to keep in mind.

* The minimum supported image size is around 256 bytes, so an image with
  less size will fail to be compressed.
* The maximum datatype precision is of 16 bits.
* Although floats from 16 or fewer bits of precision seem to work, we
  recommend using integer data when possible.

## More examples

See the [examples](examples/) directory for more examples.

## Thanks

Thanks to Marta Iborra, from the Blosc Development Team, for doing most of the job in making this plugin possible, and J. David Ibáñez and Francesc Alted for the initial contributions.  Also, thanks to Aaron Boxer, the original author of the [grok library](https://github.com/GrokImageCompression/grok), for his help in ironing out issues for making this interaction possible. 

That's all folks!

The Blosc Development Team
