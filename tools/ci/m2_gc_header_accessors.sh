#!/bin/sh
# Guard that C-side GC header users go through lj_obj_* accessors.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

hits=$(
  cd "$ROOT/src" && \
  rg -n "gch\\.marked|gch\\.nextgc|->marked|->nextgc" . \
    --glob '*.c' --glob '*.h' \
    --glob '!lj_obj.h' \
    --glob '!lj_asm_x86.h' \
    --glob '!host/*' || true
)

if [ -n "$hits" ]; then
  printf '%s\n' "$hits"
  echo "guardrail: direct C-side GC header access outside whitelist" >&2
  exit 1
fi

echo "guardrail: C-side GC header accessors clean"
