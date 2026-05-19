##############################################################################
# blosc2_htj2k: Grok (JPEG2000 codec) plugin for Blosc2
#
# Copyright (c) 2023  The Blosc Development Team <blosc@blosc.org>
# https://blosc.org
# License: GNU Affero General Public License v3.0 (see LICENSE.txt)
##############################################################################

import ctypes
import ctypes.util
import json
import os
import platform
import subprocess
import sys
import tempfile
from enum import Enum
from pathlib import Path
import atexit
import numpy as np

__version__ = "0.3.6.dev0"

CODEC_NAME = "htj2k"
CODEC_ID = 40

# On Windows, pre-load blosc2.dll before loading blosc2_htj2k.dll
if platform.system() == "Windows":
    try:
        import blosc2
        blosc2_dir = Path(blosc2.__file__).parent
        site_packages = blosc2_dir.parent  # site-packages directory
        
        # Add directories to DLL search path (prefer PEP 427 layout)
        if hasattr(os, 'add_dll_directory'):
            # PEP 427 compliant: blosc2/lib/
            blosc2_lib = blosc2_dir / "lib"
            if blosc2_lib.exists():
                os.add_dll_directory(str(blosc2_lib))

            # Common wheel layout: blosc2/.libs/
            blosc2_libs = blosc2_dir / ".libs"
            if blosc2_libs.exists():
                os.add_dll_directory(str(blosc2_libs))
            
            # Legacy (non-PEP 427): site-packages/lib/
            legacy_lib = site_packages / "lib"
            if legacy_lib.exists():
                os.add_dll_directory(str(legacy_lib))

            # Legacy: site-packages/.libs/
            legacy_libs = site_packages / ".libs"
            if legacy_libs.exists():
                os.add_dll_directory(str(legacy_libs))
            
            # Also add site-packages itself for legacy DLLs
            os.add_dll_directory(str(site_packages))
        else:
            # Fallback for older Python: extend PATH for DLL search
            path_entries = []
            blosc2_lib = blosc2_dir / "lib"
            if blosc2_lib.exists():
                path_entries.append(str(blosc2_lib))
            blosc2_libs = blosc2_dir / ".libs"
            if blosc2_libs.exists():
                path_entries.append(str(blosc2_libs))
            legacy_lib = site_packages / "lib"
            if legacy_lib.exists():
                path_entries.append(str(legacy_lib))
            legacy_libs = site_packages / ".libs"
            if legacy_libs.exists():
                path_entries.append(str(legacy_libs))
            if path_entries:
                os.environ["PATH"] = os.pathsep.join(path_entries + [os.environ.get("PATH", "")])
        
        # Pre-load blosc2 DLL - try all possible locations (prefer PEP 427 layout)
        for dll_name in ['blosc2.dll', 'libblosc2.dll']:
            # PEP 427 compliant: blosc2/blosc2.dll
            dll_path = blosc2_dir / dll_name
            if dll_path.exists():
                ctypes.CDLL(str(dll_path))
                break
            # PEP 427 compliant: blosc2/lib/blosc2.dll
            dll_path = blosc2_dir / 'lib' / dll_name
            if dll_path.exists():
                ctypes.CDLL(str(dll_path))
                break
            # Common wheel layout: blosc2/.libs/blosc2.dll
            dll_path = blosc2_dir / '.libs' / dll_name
            if dll_path.exists():
                ctypes.CDLL(str(dll_path))
                break
            # Legacy: site-packages/blosc2.dll
            dll_path = site_packages / dll_name
            if dll_path.exists():
                ctypes.CDLL(str(dll_path))
                break
            # Legacy: site-packages/lib/blosc2.dll
            dll_path = site_packages / 'lib' / dll_name
            if dll_path.exists():
                ctypes.CDLL(str(dll_path))
                break
            # Legacy: site-packages/.libs/blosc2.dll
            dll_path = site_packages / '.libs' / dll_name
            if dll_path.exists():
                ctypes.CDLL(str(dll_path))
                break
    except (ImportError, OSError):
        pass
elif platform.system() == "Linux":
    try:
        import importlib.util
        spec = importlib.util.find_spec("blosc2")
        if spec is None or spec.origin is None:
            raise ImportError("blosc2 not found")
        blosc2_dir = Path(spec.origin).parent
        site_packages = blosc2_dir.parent  # site-packages directory

        lib_dirs = [
            blosc2_dir / "lib",    # PEP 427
            blosc2_dir / ".libs",  # common wheel layout
            site_packages / "lib",  # legacy
            site_packages / ".libs",  # legacy
            blosc2_dir,  # fallback
        ]
        lib_dirs.extend(Path(p) for p in os.environ.get("LD_LIBRARY_PATH", "").split(os.pathsep) if p)
        lib_candidates = []
        for lib_dir in lib_dirs:
            if not lib_dir.exists():
                continue
            lib_candidates.extend(sorted(lib_dir.glob("libblosc2.so*")))

        loaded = False
        for lib_path in lib_candidates:
            try:
                ctypes.CDLL(str(lib_path), mode=ctypes.RTLD_GLOBAL)
                loaded = True
                break
            except OSError:
                continue
        if not loaded:
            libname = ctypes.util.find_library("blosc2")
            if libname:
                ctypes.CDLL(libname, mode=ctypes.RTLD_GLOBAL)
    except (ImportError, OSError):
        pass


class GrkFileFmt(Enum):
    """
    Supported file formats in grok.
    """

    GRK_FMT_UNK = 0
    GRK_FMT_J2K = 1
    GRK_FMT_JP2 = 2
    GRK_FMT_PXM = 3
    GRK_FMT_PGX = 4
    GRK_FMT_PAM = 5
    GRK_FMT_BMP = 6
    GRK_FMT_TIF = 7
    GRK_FMT_RAW = 8  # Big Endian
    GRK_FMT_PNG = 9
    GRK_FMT_RAWL = 10  # Little Endian
    GRK_FMT_JPG = 11


class GrkRateControl(Enum):
    """
    Available grok rate control algorithms.
    """

    BISECT = 0  # bisect with all truncation points
    PCRD_OPT = 1  # bisect with only feasible truncation points


class GrkProfile(Enum):
    """
    Available grok profiles.
    """

    GRK_PROFILE_NONE = 0x0000
    GRK_PROFILE_0 = 0x0001
    GRK_PROFILE_1 = 0x0002
    GRK_PROFILE_CINEMA_2K = 0x0003
    GRK_PROFILE_CINEMA_4K = 0x0004
    GRK_PROFILE_CINEMA_S2K = 0x0005
    GRK_PROFILE_CINEMA_S4K = 0x0006
    GRK_PROFILE_CINEMA_LTS = 0x0007
    GRK_PROFILE_BC_SINGLE = 0x0100  # Has to be combined with the target level (3-0 LSB, value between 0 and 11)
    GRK_PROFILE_BC_MULTI = 0x0200  # Has to be combined with the target level (3-0 LSB, value between 0 and 11)
    GRK_PROFILE_BC_MULTI_R = 0x0300  # Has to be combined with the target level (3-0 LSB, value between 0 and 11)
    GRK_PROFILE_BC_MASK = 0x030F  # Has to be combined with the target level (3-0 LSB, value between 0 and 11)
    GRK_PROFILE_IMF_2K = 0x0400  # Has to be combined with the target main-level (3-0 LSB, value between 0 and 11)
    # and sub-level (7-4 LSB, value between 0 and 9)
    GRK_PROFILE_IMF_4K = 0x0500  # Has to be combined with the target main-level (3-0 LSB, value between 0 and 11)
    # and sub-level (7-4 LSB, value between 0 and 9)
    GRK_PROFILE_IMF_8K = 0x0600  # Has to be combined with the target main-level (3-0 LSB, value between 0 and 11)
    # and sub-level (7-4 LSB, value between 0 and 9)
    GRK_PROFILE_IMF_2K_R = 0x0700  # Has to be combined with the target main-level (3-0 LSB, value between 0 and 11)
    # and sub-level (7-4 LSB, value between 0 and 9)
    GRK_PROFILE_IMF_4K_R = 0x0800  # Has to be combined with the target main-level (3-0 LSB, value between 0 and 11)
    # and sub-level (7-4 LSB, value between 0 and 9)
    GRK_PROFILE_IMF_8K_R = 0x0900  # Has to be combined with the target main-level (3-0 LSB, value between 0 and 11)
    # and sub-level (7-4 LSB, value between 0 and 9)
    GRK_PROFILE_MASK = 0x0FFF
    GRK_PROFILE_PART2 = 0x8000  # Must be combined with extensions
    GRK_PROFILE_PART2_EXTENSIONS_MASK = 0x3FFF


class GrkMode(Enum):
    """
    Available grok modes (aka codeblock styles).
    """

    DEFAULT = 0x000
    LAZY = 0x001  # Selective arithmetic coding bypass
    RESET = 0x002  # Reset context probabilities on coding pass boundaries
    TERMALL = 0x004  # Termination on each coding pass
    VSC = 0x008  # Vertical stripe causal context
    PTERM = 0x010  # Predictable termination
    SEGSYM = 0x020  # Segmentation symbols are used
    HT = 0x040  # high throughput block coding
    HT_MIXED = 0x080  # high throughput block coding - mixed
    HT_PHLD = 0x100  # high throughput block coding - placeholder
    JPH_RSIZ_FLAG = 0x4000  # for JPH, bit 14 of RSIZ must be set to 1


def get_libpath():
    system = platform.system()
    if system == "Linux":
        candidates = ["libblosc2_htj2k.so"]
    elif system == "Darwin":
        candidates = ["libblosc2_htj2k.dylib", "libblosc2_htj2k.so"]
    elif system == "Windows":
        candidates = [
            "blosc2_htj2k.dll",
            "libblosc2_htj2k.dll",
            "blosc2_htj2k.pyd",
            "libblosc2_htj2k.pyd",
        ]
    else:
        raise RuntimeError("Unsupported system: ", system)

    pkg_dir = Path(__file__).parent
    for libname in candidates:
        libpath = pkg_dir / libname
        if libpath.exists():
            return os.path.abspath(libpath)

    if system == "Darwin":
        for pattern in ("libblosc2_htj2k*.dylib", "libblosc2_htj2k*.so"):
            for alt_path in pkg_dir.glob(pattern):
                return os.path.abspath(alt_path)
    if system == "Windows":
        for alt_path in pkg_dir.glob("blosc2_htj2k*.pyd"):
            return os.path.abspath(alt_path)
    return os.path.abspath(pkg_dir / candidates[0])


_dll_dir_handles = []


def _add_windows_blosc2_dll_dirs():
    if platform.system() != "Windows":
        return
    if not hasattr(os, "add_dll_directory"):
        return
    debug = os.environ.get("BLOSC2_HTJ2K_DEBUG_DLL") == "1"
    site_dir = Path(__file__).resolve().parent.parent
    candidates = [
        site_dir / "bin",
        site_dir / "blosc2" / "bin",
        site_dir / "blosc2" / "lib",
        site_dir / "lib",
    ]
    if debug:
        print("blosc2_htj2k: probing DLL dirs:", file=sys.stderr)
    for cand in candidates:
        if cand.is_dir():
            try:
                # Keep handle alive; otherwise the dir is removed.
                _dll_dir_handles.append(os.add_dll_directory(str(cand)))
                if debug:
                    print(f"  added: {cand}", file=sys.stderr)
            except OSError:
                # Ignore invalid/permission issues; we'll fail later if needed.
                if debug:
                    print(f"  failed: {cand}", file=sys.stderr)
                pass


_add_windows_blosc2_dll_dirs()
libpath = get_libpath()
if os.name == "posix":
    # HDF5's Blosc2 filter may discover this codec through dlopen() later in
    # the same process.  Loading the codec globally makes `import blosc2_htj2k`
    # a real preload step, so HDF5 and the backend plugins see the same codec
    # library and C-Blosc2 dependency.
    lib = ctypes.CDLL(libpath, mode=ctypes.RTLD_GLOBAL)
else:
    lib = ctypes.cdll.LoadLibrary(libpath)


def print_libpath():
    print(libpath, end="")


class _RuntimeConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("plugin_path", ctypes.c_char_p),
        ("backend", ctypes.c_char_p),
        ("float_flags", ctypes.c_uint32),
        ("float_quant_bits", ctypes.c_uint32),
        ("float_clamp_min", ctypes.c_double),
        ("float_clamp_max", ctypes.c_double),
        ("float_nan_policy", ctypes.c_uint32),
    ]


lib.blosc2_htj2k_configure.argtypes = [ctypes.POINTER(_RuntimeConfig)]
lib.blosc2_htj2k_configure.restype = ctypes.c_int
lib.blosc2_htj2k_list_plugins.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
lib.blosc2_htj2k_list_plugins.restype = ctypes.c_int
lib.blosc2_htj2k_diagnose.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
lib.blosc2_htj2k_diagnose.restype = ctypes.c_int
lib.blosc2_htj2k_last_error.argtypes = []
lib.blosc2_htj2k_last_error.restype = ctypes.c_char_p
lib.blosc2_htj2k_register_codec.argtypes = []
lib.blosc2_htj2k_register_codec.restype = ctypes.c_int


def _optional_bytes(value):
    if value is None:
        return None
    return str(value).encode("utf-8")


def last_error():
    value = lib.blosc2_htj2k_last_error()
    return value.decode("utf-8") if value else ""


def register_codec():
    """Verify that the active Blosc2 runtime knows the official HTJ2K codec id."""
    rc = lib.blosc2_htj2k_register_codec()
    if rc != 0:
        raise RuntimeError(last_error() or "failed to register blosc2_htj2k codec")


def _parse_float_mode(value):
    if value is None:
        return 0, 0
    text = str(value).lower()
    if text in {"off", "0", "false", "disable", "disabled"}:
        return 1, 0
    if text in {"8", "uint8", "u8"}:
        return 1, 8
    if text in {"16", "uint16", "u16"}:
        return 1, 16
    if text in {"32", "uint32", "u32"}:
        return 1, 32
    raise ValueError("float_mode must be one of off, uint8, uint16 or uint32")


def configure(plugin_path=None, backend=None, htj2k_backend=None,
              float_mode=None, float_clamp_min=None, float_clamp_max=None,
              float_nan_policy="fail"):
    """Configure runtime plugin loading before first codec use.

    If ``plugin_path`` is omitted, the runtime searches the default ``plugins``
    directory installed next to ``libblosc2_htj2k``.
    """
    if backend is not None and htj2k_backend is not None and backend != htj2k_backend:
        raise ValueError("backend and htj2k_backend disagree")
    if backend is None:
        backend = htj2k_backend
    float_config_set, float_quant_bits = _parse_float_mode(float_mode)
    if float_nan_policy != "fail":
        raise ValueError("float_nan_policy='fail' is the only policy supported in v1")
    if (float_clamp_min is not None or float_clamp_max is not None) and not float_config_set:
        raise ValueError("float clamps require float_mode to be set")
    cfg = _RuntimeConfig()
    cfg.struct_size = ctypes.sizeof(_RuntimeConfig)
    cfg.plugin_path = _optional_bytes(plugin_path)
    cfg.backend = _optional_bytes(backend)
    cfg.float_flags = 0
    if float_config_set:
        cfg.float_flags |= 0x01
        cfg.float_quant_bits = float_quant_bits
    if float_clamp_min is not None:
        cfg.float_flags |= 0x02
        cfg.float_clamp_min = float(float_clamp_min)
    if float_clamp_max is not None:
        cfg.float_flags |= 0x04
        cfg.float_clamp_max = float(float_clamp_max)
    cfg.float_nan_policy = 0
    rc = lib.blosc2_htj2k_configure(ctypes.byref(cfg))
    if rc != 0:
        raise RuntimeError(last_error() or "blosc2_htj2k_configure failed")


def _read_json_from_c(func):
    needed = func(None, 0)
    if needed < 0:
        raise RuntimeError(last_error() or "blosc2_htj2k runtime query failed")
    buffer = ctypes.create_string_buffer(needed + 1)
    actual = func(buffer, len(buffer))
    if actual < 0:
        raise RuntimeError(last_error() or "blosc2_htj2k runtime query failed")
    if actual >= len(buffer):
        buffer = ctypes.create_string_buffer(actual + 1)
        actual = func(buffer, len(buffer))
        if actual < 0:
            raise RuntimeError(last_error() or "blosc2_htj2k runtime query failed")
    return json.loads(buffer.value.decode("utf-8"))


def list_plugins():
    return _read_json_from_c(lib.blosc2_htj2k_list_plugins)


def diagnose():
    return _read_json_from_c(lib.blosc2_htj2k_diagnose)


def available_backends():
    result = {"htj2k": []}
    for plugin in list_plugins().get("plugins", []):
        if plugin.get("loadable") and plugin.get("abi_valid"):
            family = plugin.get("family")
            backend = plugin.get("backend")
            if family in result and backend not in result[family]:
                result[family].append(backend)
    return result


def _plugin_root_from_record(plugin):
    path = plugin.get("path") or ""
    if not path:
        return None
    p = Path(path)
    family = plugin.get("family")
    if p.name == plugin.get("backend") and p.parent.name == family:
        return str(p.parent.parent)
    return str(p)


def _selftest_script(family, backend, plugin_root):
    clear_env = """
import os
for name in (
    "BLOSC2_HTJ2K_REPLACEMENT_DIR",
    "BLOSC2_HTJ2K_PLUGIN_PATH",
    "BLOSC2_HTJ2K_BACKEND",
):
    os.environ.pop(name, None)
"""
    configure_call = ""
    if family == "htj2k":
        configure_call = f"blosc2_htj2k.configure(plugin_path={plugin_root!r}, backend={backend!r})"

    return f"""
import blosc2
import blosc2_htj2k
import numpy as np

{clear_env}
blosc2_htj2k.register_codec()
{configure_call}

data = (np.arange(64 * 64, dtype=np.uint16).reshape(64, 64) % 4096)
cparams = {{
    "codec": blosc2_htj2k.CODEC_ID,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}}
compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
np.testing.assert_array_equal(compressed[...], data)
"""


def selftest(backends="available"):
    plugins = list_plugins().get("plugins", [])
    if backends != "available" and isinstance(backends, str):
        backends = [backends]

    broken = [
        p for p in plugins
        if p.get("exists") and (not p.get("loadable") or not p.get("abi_valid"))
    ]
    if broken and backends == "available":
        raise RuntimeError(json.dumps({"ok": False, "broken_plugins": broken}, indent=2))

    available = [
        p for p in plugins
        if p.get("family") == "htj2k" and p.get("loadable") and p.get("abi_valid")
    ]
    if backends != "available":
        requested = set(backends)
        available = [p for p in available if f"{p.get('family')}/{p.get('backend')}" in requested]

    results = []
    failed = []
    for plugin in available:
        family = plugin.get("family")
        backend = plugin.get("backend")
        plugin_root = _plugin_root_from_record(plugin)
        script = _selftest_script(family, backend, plugin_root)
        with tempfile.TemporaryDirectory() as tmpdir:
            proc = subprocess.run(
                [sys.executable, "-c", script],
                cwd=tmpdir,
                text=True,
                capture_output=True,
                check=False,
            )
        item = {
            "family": family,
            "backend": backend,
            "ok": proc.returncode == 0,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
        }
        results.append(item)
        if proc.returncode != 0:
            failed.append(item)

    summary = {"ok": not failed, "results": results}
    if failed:
        raise RuntimeError(json.dumps(summary, indent=2))
    return summary


def _main(argv=None):
    import argparse

    parser = argparse.ArgumentParser(prog="python -m blosc2_htj2k")
    parser.add_argument("--list-plugins", action="store_true")
    parser.add_argument("--diagnose", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.list_plugins:
        print(json.dumps(list_plugins(), indent=2))
    elif args.diagnose:
        print(json.dumps(diagnose(), indent=2))
    elif args.selftest:
        print(json.dumps(selftest(), indent=2))
    else:
        print_libpath()


register_codec()


# Deinitialize grok when exiting
@atexit.register
def destroy():
    lib.blosc2_htj2k_destroy()


params_defaults = {
    'tile_size': (0, 0),
    'tile_offset': (0, 0),
    # 'numlayers': 0, # blosc2_htj2k C func set_params will still receive this param
    'quality_mode': None,
    'quality_layers': np.zeros(0, dtype=np.float64),
    'numgbits': 2,
    'progression': "LRCP",
    'num_resolutions': 6,
    'codeblock_size': (64, 64),
    'mode': GrkMode.DEFAULT,
    # 10 - 19
    'irreversible': False,
    'roi_compno': -1,
    'roi_shift': 0,
    'precinct_size': (0, 0),
    'offset': (0, 0),
    'decod_format': GrkFileFmt.GRK_FMT_UNK,
    'cod_format': GrkFileFmt.GRK_FMT_JP2,
    'enableTilePartGeneration': False,
    'mct': 0,
    'max_cs_size': 0,
    # 20 - 29
    'max_comp_size': 0,
    'rsiz': GrkProfile.GRK_PROFILE_NONE,
    'framerate': 0,
    'apply_icc_': False,
    'rateControlAlgorithm': GrkRateControl.BISECT,
    'num_threads': 0,
    'deviceId': 0,
    'duration': 0,
    'repeats': 1,
    'verbose': False,
}


def set_params_defaults(**kwargs):
    """
    Set the parameters for grok.
    :param kwargs: dict
        See README.md .
    :return: None

    Warning
    -------
    If you first call this with 'cod_format' different from default
    >>> blosc2_htj2k.set_default_params({'cod_format': blosc2_htj2k.GrkFileFmt.GRK_FMT_J2K})
    and then call it again with some other parameters:
    >>> blosc2_htj2k.set_default_params({'irreversible': True})
    the default for 'cod_format' will be restored to the original blosc2_htj2k.GrkFileFmt.GRK_FMT_JP2 in grok.
    """
    # Check arguments
    not_supported = [k for k in kwargs.keys() if k not in params_defaults]
    if not_supported != []:
        raise ValueError(f"The next params are not supported: {not_supported}")

    # Prepare arguments
    params = params_defaults.copy()
    params.update(kwargs)
    args = params.values()
    args = list(args)

    # Get number of layers
    args.insert(2, 0)
    if args[3] is not None:
        args[3] = args[3].encode('utf-8')
        args[2] = args[4].shape[0]

    args[6] = args[6].encode('utf-8')

    # Convert tuples to desired NumPy arrays
    args[0] = np.array(args[0], dtype=np.int64)
    args[1] = np.array(args[1], dtype=np.int64)
    args[8] = np.array(args[8], dtype=np.int64)
    args[13] = np.array(args[13], dtype=np.int64)
    args[14] = np.array(args[14], dtype=np.int64)

    # Get value of enumerate
    args[9] = args[9].value
    args[15] = args[15].value
    args[16] = args[16].value
    args[21] = args[21].value
    args[24] = args[24].value

    if args[9] == GrkMode.HT and args[3] is not None:
        raise ValueError("High throughput mode with quality mode activated is not currently supported.")

    lib.blosc2_htj2k_set_default_params.argtypes = ([np.ctypeslib.ndpointer(dtype=np.int64)] * 2 +
                                                   [ctypes.c_int] + [ctypes.c_char_p] + [np.ctypeslib.ndpointer(dtype=np.float64)] +
                                                   [ctypes.c_int] + [ctypes.c_char_p] +
                                                   [ctypes.c_int] + [np.ctypeslib.ndpointer(dtype=np.int64)] + [ctypes.c_int] +
                                                   [ctypes.c_bool] + [ctypes.c_int] * 2 + [np.ctypeslib.ndpointer(dtype=np.int64)] +
                                                   [np.ctypeslib.ndpointer(dtype=np.int64)] +
                                                   [ctypes.c_int] +
                                                   [ctypes.c_int] + [ctypes.c_bool] +
                                                   [ctypes.c_int] * 5 + [ctypes.c_bool] +
                                                   [ctypes.c_int] * 5 + [ctypes.c_bool])

    lib.blosc2_htj2k_set_default_params(*args)


if __name__ == "__main__":
    _main()
