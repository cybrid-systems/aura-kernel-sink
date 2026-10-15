# shellcheck shell=bash
# Shared environment for running aura-kernel-sink on the Aura Soft face.
#   AURA_BIN  path to the aura binary (required unless `aura` is on PATH)
#   AURA_LIB  Aura std lib dir (default: <aura checkout>/lib, found from AURA_BIN)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AURA_BIN="${AURA_BIN:-$(command -v aura || true)}"
if [ -z "$AURA_BIN" ] || [ ! -x "$AURA_BIN" ]; then
  echo "error: set AURA_BIN to an Aura build (e.g. ~/aura/build-rel/aura)" >&2
  exit 2
fi
AURA_LIB="${AURA_LIB:-$(cd "$(dirname "$AURA_BIN")/../lib" 2>/dev/null && pwd)}"
export AURA_BIN
export AURA_LIB
export AURA_PATH="${AURA_LIB}:${REPO_ROOT}/lib${AURA_PATH:+:$AURA_PATH}"
# Soft face: sandbox off + tree-walker fallback allowed. Under the default
# Restricted face std/ffi (c-load / c-func) needs effect:ffi; see README.
export AURA_SANDBOX="${AURA_SANDBOX:-off}"
export AURA_PIPELINE_STRICT="${AURA_PIPELINE_STRICT:-0}"
export SINK_ROOT="$REPO_ROOT"
