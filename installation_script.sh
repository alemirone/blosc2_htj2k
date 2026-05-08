#!/usr/bin/env bash
set -euo pipefail

# Local development installer for blosc2_grok and optional JPEG2000 backends.
#
# Defaults:
# - uses/creates .venv unless VENV_PY or an active virtualenv is provided;
# - auto-detects ../kakadu_install;
# - builds and installs OpenHTJ2K PR #190 into ../openhtj2k_pr190_install,
#   unless BUILD_OPENHTJ2K_PR190=0 is set.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ ! -f "pyproject.toml" || ! -d "src" ]]; then
  echo "[ERROR] Run this script from the blosc2_grok repo directory." >&2
  exit 1
fi

git submodule update --init --recursive

if [[ -n "${VENV_PY:-}" ]]; then
  :
elif [[ -n "${VIRTUAL_ENV:-}" && -x "${VIRTUAL_ENV}/bin/python" ]]; then
  VENV_PY="${VIRTUAL_ENV}/bin/python"
else
  python3 -m venv .venv
  VENV_PY="$SCRIPT_DIR/.venv/bin/python"
fi
case "$VENV_PY" in
  /*) ;;
  *) VENV_PY="$SCRIPT_DIR/$VENV_PY" ;;
esac

"$VENV_PY" -m pip install --upgrade pip wheel setuptools
"$VENV_PY" -m pip install -r requirements-build.txt

# Optional Kakadu configuration.
if [[ -z "${KAKADU_ROOT:-}" ]]; then
  DEFAULT_KAKADU_ROOT="$(realpath "$SCRIPT_DIR/../kakadu_install" 2>/dev/null || true)"
  if [[ -n "$DEFAULT_KAKADU_ROOT" && -d "$DEFAULT_KAKADU_ROOT/lib" && -d "$DEFAULT_KAKADU_ROOT/managed/all_includes" ]]; then
    export KAKADU_ROOT="$DEFAULT_KAKADU_ROOT"
  fi
fi
KAKADU_ROOT="${KAKADU_ROOT:-}"
KAKADU_INCLUDE_DIR="${KAKADU_INCLUDE_DIR:-}"
KAKADU_LIB_PATH="${KAKADU_LIB_PATH:-}"
if [[ -z "$KAKADU_INCLUDE_DIR" && -n "$KAKADU_ROOT" ]]; then
  KAKADU_INCLUDE_DIR="$KAKADU_ROOT/managed/all_includes"
fi
if [[ -z "$KAKADU_LIB_PATH" && -n "$KAKADU_ROOT" ]]; then
  KAKADU_LIB_PATH="$KAKADU_ROOT/lib"
fi
if [[ -n "$KAKADU_LIB_PATH" && -d "$KAKADU_LIB_PATH" ]]; then
  export LD_LIBRARY_PATH="$KAKADU_LIB_PATH:${LD_LIBRARY_PATH:-}"
fi

# Optional OpenHTJ2K PR #190 build/install.
BUILD_OPENHTJ2K_PR190="${BUILD_OPENHTJ2K_PR190:-1}"
OPENHTJ2K_SRC_DIR="${OPENHTJ2K_SRC_DIR:-$SCRIPT_DIR/../OpenHTJ2K_PR190}"
OPENHTJ2K_BUILD_DIR="${OPENHTJ2K_BUILD_DIR:-$SCRIPT_DIR/../tmp/openhtj2k_pr190_build}"
OPENHTJ2K_INSTALL_DIR="${OPENHTJ2K_INSTALL_DIR:-$SCRIPT_DIR/../openhtj2k_pr190_install}"
OPENHTJ2K_PR_REF="${OPENHTJ2K_PR_REF:-refs/pull/190/head}"

if [[ "$BUILD_OPENHTJ2K_PR190" != "0" ]]; then
  if [[ ! -d "$OPENHTJ2K_SRC_DIR/.git" ]]; then
    git clone https://github.com/osamu620/OpenHTJ2K.git "$OPENHTJ2K_SRC_DIR"
  fi
  git -C "$OPENHTJ2K_SRC_DIR" fetch origin "$OPENHTJ2K_PR_REF:refs/remotes/origin/pr190"
  git -C "$OPENHTJ2K_SRC_DIR" checkout -B pr190 refs/remotes/origin/pr190

  cmake -S "$OPENHTJ2K_SRC_DIR" -B "$OPENHTJ2K_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$OPENHTJ2K_INSTALL_DIR" \
    -DBUILD_SHARED_LIBS=ON
  cmake --build "$OPENHTJ2K_BUILD_DIR" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-10}"
  cmake --install "$OPENHTJ2K_BUILD_DIR"

  export OPENHTJ2K_ROOT="$OPENHTJ2K_INSTALL_DIR"
elif [[ -z "${OPENHTJ2K_ROOT:-}" && -d "$OPENHTJ2K_INSTALL_DIR/lib" && -d "$OPENHTJ2K_INSTALL_DIR/include/open_htj2k/interface" ]]; then
  export OPENHTJ2K_ROOT="$OPENHTJ2K_INSTALL_DIR"
fi

OPENHTJ2K_ROOT="${OPENHTJ2K_ROOT:-}"
OPENHTJ2K_INCLUDE_DIR="${OPENHTJ2K_INCLUDE_DIR:-}"
OPENHTJ2K_LIB_PATH="${OPENHTJ2K_LIB_PATH:-}"
if [[ -z "$OPENHTJ2K_INCLUDE_DIR" && -n "$OPENHTJ2K_ROOT" ]]; then
  OPENHTJ2K_INCLUDE_DIR="$OPENHTJ2K_ROOT/include/open_htj2k/interface"
fi
if [[ -z "$OPENHTJ2K_LIB_PATH" && -n "$OPENHTJ2K_ROOT" ]]; then
  OPENHTJ2K_LIB_PATH="$OPENHTJ2K_ROOT/lib"
fi
if [[ -n "$OPENHTJ2K_LIB_PATH" && -d "$OPENHTJ2K_LIB_PATH" ]]; then
  export LD_LIBRARY_PATH="$OPENHTJ2K_LIB_PATH:${LD_LIBRARY_PATH:-}"
fi

mkdir -p "$SCRIPT_DIR/../tmp"

EXTRA_CMAKE_ARGS=()
if [[ -n "$KAKADU_ROOT" && -d "$KAKADU_INCLUDE_DIR" && -d "$KAKADU_LIB_PATH" ]]; then
  EXTRA_CMAKE_ARGS+=("-DKAKADU_ROOT=$KAKADU_ROOT")
  EXTRA_CMAKE_ARGS+=("-DKAKADU_INCLUDE_DIR=$KAKADU_INCLUDE_DIR")
  EXTRA_CMAKE_ARGS+=("-DKAKADU_LIBRARY_DIR=$KAKADU_LIB_PATH")
fi
if [[ -n "$OPENHTJ2K_ROOT" && -d "$OPENHTJ2K_INCLUDE_DIR" && -d "$OPENHTJ2K_LIB_PATH" ]]; then
  EXTRA_CMAKE_ARGS+=("-DOPENHTJ2K_ROOT=$OPENHTJ2K_ROOT")
  EXTRA_CMAKE_ARGS+=("-DOPENHTJ2K_INCLUDE_DIR=$OPENHTJ2K_INCLUDE_DIR")
  EXTRA_CMAKE_ARGS+=("-DOPENHTJ2K_LIBRARY_DIR=$OPENHTJ2K_LIB_PATH")
fi

CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-10}" \
CMAKE_ARGS="-DGROK_DISABLE_BUNDLING=ON ${EXTRA_CMAKE_ARGS[*]}" \
TMPDIR="$(realpath "$SCRIPT_DIR/../tmp")" \
"$VENV_PY" -m pip install -v --no-build-isolation .

pushd /tmp >/dev/null
"$VENV_PY" - <<'PY'
import importlib.util
import pathlib
import sys
import blosc2_grok

spec = importlib.util.find_spec("blosc2_grok")
pkg = pathlib.Path(spec.origin).parent
print("blosc2_grok import OK")
print("sys.executable:", sys.executable)
print("package dir:", pkg)
for name in ("grok", "kakadu", "openhtj2k"):
    p = pkg / "plugins" / name
    print(f"{name} plugin dir:", p, "exists=", p.is_dir())
    if p.is_dir():
        print("  libs:", sorted(x.name for x in p.iterdir() if x.suffix in {".so", ".dylib", ".dll"}))
PY
popd >/dev/null
