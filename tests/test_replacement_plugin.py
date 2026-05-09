##############################################################################
# blosc2_grok: Grok (JPEG2000 codec) plugin for Blosc2
#
# Copyright (c) 2026  The Blosc Development Team <blosc@blosc.org>
# https://blosc.org
# License: GNU Affero General Public License v3.0 (see LICENSE.txt)
##############################################################################

import subprocess
import sys
import textwrap
from importlib import metadata
from pathlib import Path

import pytest


ROUNDTRIP_SCRIPT = r"""
import blosc2
import blosc2_grok
import numpy as np

cparams = {
    "codec": blosc2.Codec.GROK,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}


def roundtrip(data):
    compressed = blosc2.asarray(
        data,
        chunks=data.shape,
        blocks=data.shape,
        cparams=cparams,
    )
    np.testing.assert_array_equal(compressed[...], data)


roundtrip((np.arange(64 * 64, dtype=np.uint16).reshape(64, 64) % 4096))

rgb = np.arange(64 * 64 * 3, dtype=np.uint16).reshape(64, 64, 3)
roundtrip(rgb % 4096)
"""

HTJ2K_UINT16_ROUNDTRIP_SCRIPT = r"""
import blosc2
import blosc2_grok
import numpy as np

blosc2_grok.set_params_defaults(mode=blosc2_grok.GrkMode.HT)

cparams = {
    "codec": blosc2.Codec.GROK,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}


def roundtrip(data):
    compressed = blosc2.asarray(
        data,
        chunks=data.shape,
        blocks=data.shape,
        cparams=cparams,
    )
    np.testing.assert_array_equal(compressed[...], data)


roundtrip((np.arange(64 * 64, dtype=np.uint16).reshape(64, 64) % 4096))

rgb = np.arange(64 * 64 * 3, dtype=np.uint16).reshape(64, 64, 3)
roundtrip(rgb % 4096)
"""

HTJ2K_UINT16_LOSSY_SCRIPT = r"""
import blosc2
import blosc2_grok
import numpy as np

blosc2_grok.set_params_defaults(mode=blosc2_grok.GrkMode.HT)

y, x = np.indices((64, 64))
data = ((x * 13 + y * 7) % 4096).astype(np.uint16)
cparams = {
    "codec": blosc2.Codec.GROK,
    "codec_meta": 20,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
decoded = compressed[...]
err = np.abs(decoded.astype(np.int32) - data.astype(np.int32))
assert decoded.shape == data.shape
assert decoded.dtype == data.dtype
assert compressed.schunk.cratio > 1.0
assert err.max() > 0
assert err.max() < 4096
assert err.mean() < 512
"""

UNKNOWN_DECODE_SCRIPT = r"""
import ctypes
import platform
from pathlib import Path

import blosc2_grok

package_dir = Path(blosc2_grok.__file__).resolve().parent
if platform.system() == "Windows":
    patterns = ("*blosc2_grok*.dll", "*.pyd")
elif platform.system() == "Darwin":
    patterns = ("libblosc2_grok*.dylib", "libblosc2_grok*.so")
else:
    patterns = ("libblosc2_grok*.so",)

libraries = []
for pattern in patterns:
    libraries.extend(package_dir.glob(pattern))

assert libraries, package_dir
lib = ctypes.CDLL(str(libraries[0]))
lib.blosc2_grok_decoder.argtypes = [
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_int32,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_int32,
    ctypes.c_uint8,
    ctypes.c_void_p,
    ctypes.c_void_p,
]
lib.blosc2_grok_decoder.restype = ctypes.c_int

inp = (ctypes.c_uint8 * 8)(1, 2, 3, 4, 5, 6, 7, 8)
out = (ctypes.c_uint8 * 64)()
rc = lib.blosc2_grok_decoder(inp, len(inp), out, len(out), 0, None, None)
assert rc == -1
"""


def run_python(script, tmp_path):
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(script)],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        check=False,
    )


def installed_plugin_dir(family, name):
    plugin_dir = Path(
        metadata.distribution("blosc2_grok").locate_file(
            f"blosc2_grok/plugins/{family}/{name}"
        )
    ).resolve()
    if not plugin_dir.is_dir():
        pytest.skip(f"{family}/{name} backend is not installed")
    suffixes = (
        {".dll", ".pyd"}
        if sys.platform == "win32"
        else {".dylib", ".so"} if sys.platform == "darwin" else {".so"}
    )
    if not any(p.suffix in suffixes for p in plugin_dir.iterdir()):
        pytest.skip(f"{family}/{name} backend has no shared library")
    return plugin_dir


def skip_if_loader_problem(result):
    if result.returncode == 0:
        return
    loader_messages = (
        "dlopen failed",
        "LoadLibrary failed",
        "No valid J2K plugin",
        "No valid HTJ2K plugin",
        "cannot open shared object file",
    )
    if any(message in result.stderr for message in loader_messages):
        pytest.skip(result.stderr)


def test_native_backend_roundtrip_without_replacement(tmp_path):
    """The normal J2K Grok path must work when no replacement is configured."""

    script = (
        textwrap.dedent(
            r"""
        import os

        os.environ.pop("BLOSC2_GROK_REPLACEMENT_DIR", None)
        os.environ.pop("BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded J2K plugin:" not in result.stderr
    assert "Loaded HTJ2K plugin:" not in result.stderr


def test_grok_j2k_replacement_backend_roundtrip(tmp_path):
    """Exercise the J2K runtime replacement path without requiring Kakadu."""

    script = (
        textwrap.dedent(
            r"""
        import os
        import platform
        from pathlib import Path

        import blosc2_grok

        plugin_dir = Path(blosc2_grok.__file__).resolve().parent / "plugins" / "j2k" / "grok"
        if platform.system() == "Windows":
            patterns = ("*.dll", "*.pyd")
        elif platform.system() == "Darwin":
            patterns = ("*.dylib", "*.so")
        else:
            patterns = ("*.so",)

        plugin_libs = []
        for pattern in patterns:
            plugin_libs.extend(plugin_dir.glob(pattern))

        assert plugin_dir.is_dir(), plugin_dir
        assert plugin_libs, plugin_dir

        os.environ["BLOSC2_GROK_REPLACEMENT_DIR"] = str(plugin_dir)
        os.environ.pop("BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded J2K plugin: grok" in result.stderr


def test_htj2k_uint16_rejected_without_htj2k_replacement(tmp_path):
    """HTJ2K must fail clearly when no HTJ2K backend is configured."""

    script = (
        textwrap.dedent(
            r"""
        import os

        os.environ.pop("BLOSC2_GROK_REPLACEMENT_DIR", None)
        os.environ.pop("BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + HTJ2K_UINT16_ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)

    assert result.returncode != 0
    assert "HTJ2K encoding requires BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR" in result.stderr


def test_unknown_codestream_rejected_without_decoder_guessing(tmp_path):
    """An unrecognized codestream must fail before any backend guessing."""

    result = run_python(UNKNOWN_DECODE_SCRIPT, tmp_path)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Could not identify JPEG2000 codestream family" in result.stderr


def test_kakadu_j2k_replacement_backend_uint16_roundtrip(tmp_path):
    """Kakadu is optional, but when installed it must handle J2K uint16."""

    plugin_dir = installed_plugin_dir("j2k", "kakadu")
    script = (
        textwrap.dedent(
            f"""
        import os

        os.environ["BLOSC2_GROK_REPLACEMENT_DIR"] = {str(plugin_dir)!r}
        os.environ.pop("BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)
    skip_if_loader_problem(result)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded J2K plugin: Kakadu" in result.stderr
    assert "Kakadu mode: J2K" in result.stderr


def test_kakadu_htj2k_replacement_backend_uint16_roundtrip(tmp_path):
    """Kakadu is optional, but when installed it must handle HTJ2K uint16."""

    plugin_dir = installed_plugin_dir("htj2k", "kakadu")
    script = (
        textwrap.dedent(
            f"""
        import os

        os.environ.pop("BLOSC2_GROK_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR"] = {str(plugin_dir)!r}
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + HTJ2K_UINT16_ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)
    skip_if_loader_problem(result)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded HTJ2K plugin: Kakadu" in result.stderr
    assert "Kakadu mode: HTJ2K" in result.stderr


def test_kakadu_htj2k_replacement_backend_uint16_lossy(tmp_path):
    """Exercise the Kakadu HTJ2K lossy path when the optional backend exists."""

    plugin_dir = installed_plugin_dir("htj2k", "kakadu")
    script = (
        textwrap.dedent(
            f"""
        import os

        os.environ.pop("BLOSC2_GROK_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR"] = {str(plugin_dir)!r}
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + HTJ2K_UINT16_LOSSY_SCRIPT
    )

    result = run_python(script, tmp_path)
    skip_if_loader_problem(result)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Kakadu mode: HTJ2K" in result.stderr


def test_openhtj2k_replacement_backend_htj2k_uint16_roundtrip(tmp_path):
    """OpenHTJ2K PR #190 or newer must support HTJ2K uint16 when installed."""

    plugin_dir = installed_plugin_dir("htj2k", "openhtj2k")
    script = (
        textwrap.dedent(
            f"""
        import os

        os.environ.pop("BLOSC2_GROK_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR"] = {str(plugin_dir)!r}
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + HTJ2K_UINT16_ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)
    skip_if_loader_problem(result)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded HTJ2K plugin: OpenHTJ2K" in result.stderr


def test_openhtj2k_replacement_backend_htj2k_uint16_lossy(tmp_path):
    """Exercise the OpenHTJ2K HTJ2K lossy path when the optional backend exists."""

    plugin_dir = installed_plugin_dir("htj2k", "openhtj2k")
    script = (
        textwrap.dedent(
            f"""
        import os

        os.environ.pop("BLOSC2_GROK_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_HTJ2K_REPLACEMENT_DIR"] = {str(plugin_dir)!r}
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + HTJ2K_UINT16_LOSSY_SCRIPT
    )

    result = run_python(script, tmp_path)
    skip_if_loader_problem(result)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded HTJ2K plugin: OpenHTJ2K" in result.stderr
