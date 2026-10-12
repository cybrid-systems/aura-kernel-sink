#!/usr/bin/env bash
# End-to-end MVP: build the kernels, then run scripts/mvp.aura in ONE Aura
# process: identify -> plan -> aot:reload + c-load -> mutate:rebind ->
# eval-current -> score -> accept | hot-strategy:heal!.
#   SINK_PLANT=real|fake   data plane (default real; fake = Aura stand-ins)
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
"$REPO_ROOT/scripts/gen_kernel.sh"
export SINK_PLANT="${SINK_PLANT:-real}"
exec "$AURA_BIN" "$REPO_ROOT/scripts/mvp.aura"
