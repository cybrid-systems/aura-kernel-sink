#!/usr/bin/env bash
# Generate C kernels from the discovered workspace defines (lib/kgen.aura)
# into build/gen/*.c, then build them with kernels/build.sh into
# build/kernels/libgen_<name>.so. Hand-written kernels/*.c stay the fallback.
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
mkdir -p build/gen
"$AURA_BIN" "$REPO_ROOT/scripts/kgen.aura"
"$REPO_ROOT/kernels/build.sh"
