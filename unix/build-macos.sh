#!/usr/bin/env bash
# Build SDLPAL for macOS (arm64) against the SDL3 already built by the
# pal-saturn-recomp workspace. No system-wide install, no sudo.
#
#   bash unix/build-macos.sh
#
# The resulting binary is unix/sdlpal. It needs a game data directory
# (map.mkf/gop.mkg/fbp.mkf/... ) pointed to by sdlpal.cfg to run.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDL3_PREFIX="${SDL3_PREFIX:-$ROOT/../pal-saturn-recomp/build/prefix}"

if [[ ! -f "$SDL3_PREFIX/lib/pkgconfig/sdl3.pc" ]]; then
  echo "SDL3 not found at $SDL3_PREFIX" >&2
  echo "Build it first (see pal-saturn-recomp) or set SDL3_PREFIX." >&2
  exit 1
fi

# Submodules must be present (libopusfile/src/config.h comes from one).
git -C "$ROOT" submodule update --init --recursive

cd "$ROOT/unix"
PKG_CONFIG_PATH="$SDL3_PREFIX/lib/pkgconfig" \
make -j"$(sysctl -n hw.ncpu)" GENERATED= \
  EXTRA_CCFLAGS="-DPAL_HAS_PLATFORM_SPECIFIC_UTILS -I. -I.. -DHAVE_CONFIG_H \
-I../libopusfile/src -I../libopusfile/celt -I../libopusfile/silk \
-I../libopusfile/silk/float -I../libopusfile/silk/fixed \
-I../liboggvorbis/src -I../liboggvorbis/include -I../libopusfile/include \
-I../timidity -I../libmad"

echo
echo "Built: $ROOT/unix/sdlpal"
file "$ROOT/unix/sdlpal"
