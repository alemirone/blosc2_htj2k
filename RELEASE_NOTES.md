# Release notes

## Changes from 0.4.1 to 0.4.2

* Serialize OpenHTJ2K backend encode/decode calls because the upstream
  OpenHTJ2K C++ API is not safe under concurrent Blosc2 block compression.
  This fixes failures for 3D stacks stored as one chunk with 2D blocks, e.g.
  `(10, 128, 128)` chunks with `(1, 128, 128)` blocks.
* Add a regression test for multithreaded OpenHTJ2K compression of a single
  3D chunk split into 2D blocks.

## Changes from 0.4.0 to 0.4.1

* Fix the OpenHTJ2K decoder wrapper so it follows the upstream buffer
  ownership contract and no longer risks double-free/corruption on multi-frame
  stacks.
* Add a regression test covering repeated roundtrips of chunked HTJ2K stacks
  with the OpenHTJ2K backend.

## Changes from 0.3.6 to 0.4.0

* Use the official Blosc2 global codec id 40 for HTJ2K.
* Require python-blosc2 4.4.3 or newer, which exposes the official HTJ2K codec
  id and removes the need for a temporary python-blosc2 branch.
* Update installation and quickstart documentation for the Blosc organization
  repositories and upstream c-blosc2/python-blosc2 packages.
* Add Trusted Publishing configuration for PyPI wheel uploads from GitHub
  Actions.
* Document the current HDF5 status: direct Blosc2 usage works with
  python-blosc2 4.4.3, while transparent HDF5 workflows need an HDF5 Blosc2
  filter built against a c-blosc2 runtime that knows codec id 40.

## Changes from 0.3.5 to 0.3.6

* Add a runtime backend plugin mechanism for J2K and HTJ2K codecs.
* Add family-specific plugin ABIs for J2K and HTJ2K backends.
* Add an always-built J2K Grok replacement backend for testing the plugin path.
* Add optional Kakadu J2K/HTJ2K backends when Kakadu headers/libraries are
  available locally.
* Add an optional OpenHTJ2K HTJ2K backend when the `uint16` C++ API available
  in OpenHTJ2K `v0.4.0` and newer is detected by CMake.
* Build OpenHTJ2K `v0.4.0` automatically during `pip install .` when no
  OpenHTJ2K installation is configured, and bundle its runtime library next to
  the HTJ2K backend plugin.
* Add explicit runtime configuration APIs:
  `blosc2_htj2k.configure()` for Python and `blosc2_htj2k_configure()` for C.
* Add plugin listing, diagnostics and self-test helpers:
  `list_plugins()`, `available_backends()`, `diagnose()`, `selftest()` and
  the corresponding `python -m blosc2_htj2k` commands.
* Preserve legacy environment variables for backend selection while adding
  named backend variables through `BLOSC2_HTJ2K_PLUGIN_PATH`,
  `BLOSC2_HTJ2K_BACKEND`.
* Improve HDF5 deployment guidance for explicit loading and `LD_PRELOAD`
  fallback use cases.
* Keep Kakadu and OpenHTJ2K out of the core codec library; optional backends
  are loaded through the plugin ABI.
* Improve Kakadu J2K lossy interoperability with Grok by avoiding too-small
  irreversible 9/7 quantization seeds for `uint16` streams.

## Changes from 0.3.4 to 0.3.5

* Bug fix for switched row/col dimensions (PR #20, thanks to @alemirone)

## Changes from 0.3.3 to 0.3.4

* Implement PEP 427 wheel layout format and general cleanup of installation
* Changes to standardise CI/CD workflow

## Changes from 0.3.2 to 0.3.3

* Change the Python extension from MODULE to SHARED on some
  platforms (Linux and MacOSX/arm64; the rest do not seem
  to support SHARED mode).  This allows for a C program to
  use the plugin as a shared library.

## Changes from 0.3.1 to 0.3.2

* Support for arbitrary numbers of leading 1 dimensions in the input data.
  This is common in image data where the leading dimensions are reserved for
  stacks of images.

## Changes from 0.3.0 to 0.3.1

* Build aarch64 wheels.


## Changes from 0.2.3 to 0.3.0

* Support specifying the `rates` value as the
 `codec_meta` from the blosc2 cparams.


## Changes from 0.2.2 to 0.2.3

* Fixed a memory leak in the decoder function.


## Changes from 0.2.1 to 0.2.2

* Changed initialization of the grok library
  to first time it is used. This evicts having to import
  the `blosc2-htj2k` package to use the plugin.


## Changes from 0.2.0 to 0.2.1

* Avoid calling `set_params_defaults` for setting own blosc2_htj2k defaults.


## Changes from 0.1.0 to 0.2.0

* Default `cod_format` changed to JP2.
* Added `mode` param to perform high throughput coding.
* Added some benchmarks.
* Added include header in `utils.h`.


## Changes from 0.0.1 to 0.1.0

* Added support for many parameters for the grok codec.
* Documentation for params added in the README.
* Fixed a bug when compressing several images in a row.
* Sporadic segfaults when compressing/decompressing fixed.
* First public release.
