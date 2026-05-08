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

KAKADU_HTJ2K_LOSSY_SCRIPT = r"""
import blosc2
import blosc2_grok
import numpy as np

blosc2_grok.set_params_defaults(mode=blosc2_grok.GrkMode.HT)

data = (np.indices((64, 64)).sum(axis=0).astype(np.uint16) * 8) % 4096
cparams = {
    "codec": blosc2.Codec.GROK,
    "codec_meta": 20,
    "filters": [],
    "splitmode": blosc2.SplitMode.NEVER_SPLIT,
}
compressed = blosc2.asarray(data, chunks=data.shape, blocks=data.shape, cparams=cparams)
decoded = compressed[...]
assert decoded.shape == data.shape
assert decoded.dtype == data.dtype
assert compressed.schunk.cratio > 1.0
"""


def run_python(script, tmp_path):
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(script)],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        check=False,
    )


def installed_plugin_dir(name):
    plugin_dir = Path(
        metadata.distribution("blosc2_grok").locate_file(f"blosc2_grok/plugins/{name}")
    ).resolve()
    if not plugin_dir.is_dir():
        pytest.skip(f"{name} backend is not installed")
    suffixes = {".dll", ".pyd"} if sys.platform == "win32" else {".dylib", ".so"} if sys.platform == "darwin" else {".so"}
    if not any(p.suffix in suffixes for p in plugin_dir.iterdir()):
        pytest.skip(f"{name} backend has no shared library")
    return plugin_dir


def test_native_backend_roundtrip_without_replacement(tmp_path):
    """The normal Grok path must keep working when no replacement is configured."""

    script = (
        textwrap.dedent(
            r"""
        import os

        os.environ.pop("BLOSC2_GROK_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded plugin:" not in result.stderr


def test_grok_replacement_backend_roundtrip(tmp_path):
    """Exercise the runtime replacement path without requiring Kakadu."""

    script = (
        textwrap.dedent(
        r"""
        import os
        import platform
        from pathlib import Path

        import blosc2_grok

        plugin_dir = Path(blosc2_grok.__file__).resolve().parent / "plugins" / "grok"
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
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded plugin: grok" in result.stderr


def test_native_htj2k_uint16_rejected_without_replacement(tmp_path):
    """HTJ2K uint16 must not silently use the native Grok fallback."""

    script = (
        textwrap.dedent(
            r"""
        import os

        os.environ.pop("BLOSC2_GROK_REPLACEMENT_DIR", None)
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + HTJ2K_UINT16_ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)

    assert result.returncode != 0
    assert "Native Grok HTJ2K uint16 is not enabled" in result.stderr


def test_kakadu_replacement_backend_htj2k_uint16_roundtrip(tmp_path):
    """Kakadu is optional, but when installed it must handle HTJ2K uint16."""

    plugin_dir = installed_plugin_dir("kakadu")
    script = (
        textwrap.dedent(
            f"""
        import os

        os.environ["BLOSC2_GROK_REPLACEMENT_DIR"] = {str(plugin_dir)!r}
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + HTJ2K_UINT16_ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)
    if result.returncode != 0 and ("dlopen failed" in result.stderr or "No valid plugin" in result.stderr):
        pytest.skip(result.stderr)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded plugin: Kakadu-J2K" in result.stderr
    assert "Kakadu mode: HTJ2K" in result.stderr


def test_kakadu_replacement_backend_htj2k_uint16_lossy(tmp_path):
    """Exercise the Kakadu HTJ2K lossy path when the optional backend exists."""

    plugin_dir = installed_plugin_dir("kakadu")
    script = (
        textwrap.dedent(
            f"""
        import os

        os.environ["BLOSC2_GROK_REPLACEMENT_DIR"] = {str(plugin_dir)!r}
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + KAKADU_HTJ2K_LOSSY_SCRIPT
    )

    result = run_python(script, tmp_path)
    if result.returncode != 0 and ("dlopen failed" in result.stderr or "No valid plugin" in result.stderr):
        pytest.skip(result.stderr)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Kakadu mode: HTJ2K" in result.stderr


def test_openhtj2k_replacement_backend_htj2k_uint16_roundtrip(tmp_path):
    """OpenHTJ2K PR #190 or newer must support HTJ2K uint16 when installed."""

    plugin_dir = installed_plugin_dir("openhtj2k")
    script = (
        textwrap.dedent(
            f"""
        import os

        os.environ["BLOSC2_GROK_REPLACEMENT_DIR"] = {str(plugin_dir)!r}
        os.environ["BLOSC2_GROK_DEBUG"] = "1"
        """
        )
        + HTJ2K_UINT16_ROUNDTRIP_SCRIPT
    )

    result = run_python(script, tmp_path)
    if result.returncode != 0 and ("dlopen failed" in result.stderr or "No valid plugin" in result.stderr):
        pytest.skip(result.stderr)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "Loaded plugin: OpenHTJ2K" in result.stderr
