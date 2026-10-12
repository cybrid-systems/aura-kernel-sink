#!/usr/bin/env bash
# Build every kernels/*.c into build/kernels/lib<name>.so.
#   CC      C compiler (default: cc)
#   CFLAGS  extra flags (default: -O2)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
out="$root/build/kernels"
mkdir -p "$out"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2}"
for src in "$here"/*.c; do
  name="$(basename "$src" .c)"
  so="$out/lib$name.so"
  if [ ! -f "$so" ] || [ "$src" -nt "$so" ] || [ "$here/sink_abi.h" -nt "$so" ]; then
    # shellcheck disable=SC2086
    "$CC" $CFLAGS -std=c11 -Wall -Wextra -Werror -shared -fPIC -fvisibility=hidden \
      -I"$here" -o "$so" "$src"
    echo "built build/kernels/lib$name.so"
  fi
done
