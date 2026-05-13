# blosc2_grok

For using blosc2_grok you will first have to create and install its wheel.


## Download the repository

```shell
git clone https://github.com/Blosc/blosc2_grok.git
cd blosc2_grok
git submodule update --init
```

## Create the wheel

For Linux:

```shell
CMAKE_BUILD_PARALLEL_LEVEL=10 python -m cibuildwheel --only 'cp311-manylinux_x86_64'
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
pip install wheelhouse/blosc2_grok-*.whl --force-reinstall
```

## Compiling C-Blosc2 apps with the grok plugin

The blosc2_grok wheel includes static libraries and headers for reference, but for compiling C applications with the grok plugin, you should build and install blosc2_grok from source rather than using the Python wheel.

### Building from source for C development

```bash
git clone https://github.com/Blosc/blosc2_grok.git
cd blosc2_grok
git submodule update --init
mkdir build && cd build
cmake ..
make
sudo make install
```

Then compile your C application:

```bash
gcc myapp.c -I/usr/local/include -L/usr/local/lib -lblosc2_grok -lgrokj2k -lblosc2 -o myapp
```

## HDF5 loader order

When `blosc2_grok` is used through the HDF5 Blosc2 filter, load the
`libblosc2_grok` shared library before the first HDF5 read or write.  Configure
runtime backends before the first codec use.  Python code can do both steps
explicitly:

```python
import blosc2_grok
import hdf5plugin
import h5py

blosc2_grok.configure(
    plugin_path="/path/to/site-packages/blosc2_grok/plugins",
    j2k_backend="grok",
    htj2k_backend="openhtj2k",
)
```

For C/C++ applications, the preferred approach is to link or explicitly load
`libblosc2_grok` and call `blosc2_grok_configure()` before opening HDF5 data:

```c
#include "blosc2_grok_public.h"

blosc2_grok_runtime_config cfg = {0};
cfg.struct_size = sizeof(cfg);
cfg.plugin_path = "/path/to/site-packages/blosc2_grok/plugins";
cfg.j2k_backend = "grok";
cfg.htj2k_backend = "openhtj2k";

if (blosc2_grok_configure(&cfg) != 0) {
    fprintf(stderr, "%s\n", blosc2_grok_last_error());
}
```

For viewers or other non-Python hosts where no explicit initialization hook is
available, preload the codec library before process startup:

```bash
export BLOSC2_GROK_LIBRARY=/path/to/libblosc2_grok.so
export LD_PRELOAD="${BLOSC2_GROK_LIBRARY}${LD_PRELOAD:+:$LD_PRELOAD}"
```

Backend selection can also be configured without code through environment
variables.  If no explicit API call has been made, legacy direct-directory
variables have priority over named backend variables:

```bash
export BLOSC2_GROK_REPLACEMENT_DIR=/path/to/plugins/j2k/grok
export BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR=/path/to/plugins/htj2k/openhtj2k
```

or:

```bash
export BLOSC2_GROK_PLUGIN_PATH=/path/to/site-packages/blosc2_grok/plugins
export BLOSC2_GROK_J2K_BACKEND=grok
export BLOSC2_GROK_HTJ2K_BACKEND=openhtj2k
```

Use the diagnostic helpers to verify what the runtime sees:

```bash
python -m blosc2_grok --list-plugins
python -m blosc2_grok --diagnose
python -m blosc2_grok --selftest
```

## Debugging

If you would like to debug and run an example from C getting to track the problem through the C functions, you can use
the codec as a local registered codec. For that you will have to do the following:

```
// In blosc2_grok_public.h
// Comment out the info
//BLOSC2_GROK_EXPORT codec_info info = {
//    .encoder=(char *)"blosc2_grok_encoder",
//    .decoder=(char *)"blosc2_grok_decoder"
//};

// In your example, include the blosc2_grok_public.h header and add the function pointers
// to the codec struct before registering it.
#include "blosc2_grok_public.h"
// Some code in between
blosc2_codec grok_codec = {0};
grok_codec.compname = (char *)"grok";
grok_codec.compcode = 160;
grok_codec.complib = 1;
grok_codec.version = 0;
grok_codec.encoder = &blosc2_grok_encoder;
grok_codec.decoder = &blosc2_grok_decoder;
int rc = blosc2_register_codec(&grok_codec);
```

That's all folks!
