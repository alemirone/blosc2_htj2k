# blosc2_htj2k

For using blosc2_htj2k you will first have to create and install its wheel.


## Download the repository

```shell
git clone https://github.com/Blosc/blosc2_htj2k.git
cd blosc2_htj2k
git submodule update --init
```

## Create the wheel

For Linux:

```shell
CMAKE_BUILD_PARALLEL_LEVEL=10 python -m cibuildwheel --only 'cp311-manylinux_x86_64'
```

By default the build downloads and compiles OpenHTJ2K PR190 when no
OpenHTJ2K installation is provided.  To require a pre-existing OpenHTJ2K
installation instead, pass:

```shell
CMAKE_ARGS="-DBLOSC2_HTJ2K_BUILD_OPENHTJ2K=OFF" python -m pip install -v --no-build-isolation .
```

To force a local OpenHTJ2K installation:

```shell
OPENHTJ2K_ROOT=/path/to/openhtj2k/install \
OPENHTJ2K_INCLUDE_DIR=/path/to/openhtj2k/install/include/open_htj2k/interface \
OPENHTJ2K_LIB_PATH=/path/to/openhtj2k/install/lib \
python -m pip install -v --no-build-isolation .
```

For Mac x86_64:

```shell
CMAKE_BUILD_PARALLEL_LEVEL=10 CMAKE_OSX_ARCHITECTURES=x86_64 python -m cibuildwheel --only 'cp311-macosx_x86_64'
```

For Mac arm64:

```shell
CMAKE_BUILD_PARALLEL_LEVEL=10 CMAKE_OSX_ARCHITECTURES=arm64 python -m cibuildwheel --only 'cp311-macosx_arm64'
```

## Install the wheel

```shell
pip install wheelhouse/blosc2_htj2k-*.whl --force-reinstall
```

## Compiling C-Blosc2 apps with the HTJ2K plugin

The blosc2_htj2k wheel includes static libraries and headers for reference, but for compiling C applications with the HTJ2K plugin, you should build and install blosc2_htj2k from source rather than using the Python wheel.

### Building from source for C development

```bash
git clone https://github.com/Blosc/blosc2_htj2k.git
cd blosc2_htj2k
git submodule update --init
mkdir build && cd build
cmake ..
make
sudo make install
```

Then compile your C application:

```bash
gcc myapp.c -I/usr/local/include -L/usr/local/lib -lblosc2_htj2k -lblosc2 -o myapp
```

## HDF5 loader order

When `blosc2_htj2k` is used through the HDF5 Blosc2 filter, load the
`libblosc2_htj2k` shared library before the first HDF5 read or write.  Configure
runtime backends before the first codec use.  Python code can do both steps
explicitly:

```python
import blosc2_htj2k
import hdf5plugin
import h5py

blosc2_htj2k.configure(
    backend="openhtj2k",
)
```

`plugin_path` is optional for plugins installed in the package default location
next to `libblosc2_htj2k`.  Pass it only for custom plugin roots.

For C/C++ applications, the preferred approach is to link or explicitly load
`libblosc2_htj2k` and call `blosc2_htj2k_configure()` before opening HDF5 data:

```c
#include "blosc2_htj2k_public.h"

blosc2_htj2k_runtime_config cfg = {0};
cfg.struct_size = sizeof(cfg);
cfg.backend = "openhtj2k";

if (blosc2_htj2k_configure(&cfg) != 0) {
    fprintf(stderr, "%s\n", blosc2_htj2k_last_error());
}
```

For viewers or other non-Python hosts where no explicit initialization hook is
available, preload the codec library before process startup:

```bash
export BLOSC2_HTJ2K_LIBRARY=/path/to/libblosc2_htj2k.so
export LD_PRELOAD="${BLOSC2_HTJ2K_LIBRARY}${LD_PRELOAD:+:$LD_PRELOAD}"
```

Backend selection can also be configured without code through environment
variables.  If no explicit API call has been made, legacy direct-directory
variables have priority over named backend variables:

```bash
export BLOSC2_HTJ2K_REPLACEMENT_DIR=/path/to/plugins/htj2k/openhtj2k
```

or:

```bash
export BLOSC2_HTJ2K_BACKEND=openhtj2k
```

If neither explicit API nor environment variables select a backend, the
installed `blosc2_htj2k_plugins.json` manifest is used.  The packaged manifest
prefers Kakadu for HTJ2K when the Kakadu plugin is installed, then falls back to
the OpenHTJ2K backend:

```json
{
  "priority": {
    "htj2k": ["kakadu", "openhtj2k"]
  }
}
```

`BLOSC2_HTJ2K_PLUGIN_PATH` is optional when the plugins are installed under the
default package root; the runtime will also search the `plugins` directory next
to `libblosc2_htj2k`.

Use the diagnostic helpers to verify what the runtime sees:

```bash
python -m blosc2_htj2k --list-plugins
python -m blosc2_htj2k --diagnose
python -m blosc2_htj2k --selftest
```

## Debugging

If you would like to debug and run an example from C, first make sure the active
c-blosc2 build knows the official HTJ2K id:

```
#include "blosc2_htj2k_public.h"

blosc2_init();
if (blosc2_compname_to_compcode("htj2k") != BLOSC2_HTJ2K_CODEC_ID) {
    /* Use a c-blosc2 build that contains BLOSC_CODEC_HTJ2K = 40. */
    return 1;
}
if (blosc2_htj2k_register_codec() != 0) {
    fprintf(stderr, "%s\n", blosc2_htj2k_last_error());
    return 1;
}
```

The public `blosc2_register_codec()` API is only for user codecs in the dynamic
range starting at 160.  It cannot add official global ids such as 40 to an older
c-blosc2 runtime.

That's all folks!
