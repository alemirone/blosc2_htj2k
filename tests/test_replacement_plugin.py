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


def run_python(script, tmp_path):
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(script)],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        check=False,
    )


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
