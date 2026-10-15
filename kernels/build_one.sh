#!/usr/bin/env bash
# Build ONE generated kernel: build_one.sh <src.c> <out.so>
# Same flags as kernels/build.sh (generated sources). Writes <out.so>.tmp and
# renames it into place, so a reader polling for <out.so> never sees a
# half-written file. Used by lib/bg.aura (background compile).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$1"; so="$2"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2}"
AURA_LIB="${AURA_LIB:-$(cd "$(dirname "${AURA_BIN:-/nonexistent}")/../lib" 2>/dev/null && pwd || true)}"
if [ -z "$AURA_LIB" ] || [ ! -f "$AURA_LIB/aura_value_abi.h" ]; then
  echo "error: AURA_LIB must point at an Aura lib/ with aura_value_abi.h" >&2
  exit 2
fi
mkdir -p "$(dirname "$so")"
# shellcheck disable=SC2086
"$CC" $CFLAGS -std=c11 -Wall -Wextra -Werror -Wno-unused-label -Wno-unused-variable \
  -shared -fPIC -fvisibility=hidden -I"$here" -I"$AURA_LIB" -Wl,-Bsymbolic -o "$so.tmp" "$src" -lm
mv -f "$so.tmp" "$so"
