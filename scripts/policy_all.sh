#!/usr/bin/env bash
# One Aura process per kernel (SINK_ONLY), all appending to the same
# trajectory. Same env knobs as scripts/policy.aura (SINK_PROPOSAL, SINK_ROUND,
# SINK_TRAJECTORY, SINK_MIN_SPEEDUP, SINK_MIN_SOFT_US, SINK_SCORE_ROUNDS).
# Separate processes because c-func ids accumulate across sinks in one
# process and collide with closure ids (aura#4532): in a single process the
# 7th/8th sink grew RSS to 13 GB.
set -uo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
export SINK_TRAJECTORY="${SINK_TRAJECTORY:-build/trajectory.jsonl}"
mkdir -p "$(dirname "$SINK_TRAJECTORY")"
KERNELS="${SINK_KERNELS:-vsum vdot vnorm2 vscaled vcount-above vmaxabs vsum-short vdot-short vbad-mix}"
status=0
for k in $KERNELS; do
  out=$(SINK_ONLY="$k" "$AURA_BIN" scripts/policy.aura 2>&1); rc=$?
  grep -a '\[policy\]' <<<"$out"
  if [ $rc -ne 0 ] || ! grep -q '\[policy\]' <<<"$out"; then
    echo "policy_all: $k failed (rc=$rc)" >&2; echo "$out" | tail -5 >&2; status=1
  fi
done
for d in KEEP ROLLBACK ABORT SKIP; do printf '%s %s ' "$d" "$(grep -c "\"decision\":\"$d\"" "$SINK_TRAJECTORY" || true)"; done; echo
exit $status
