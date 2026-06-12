#!/bin/sh
# Guard M5 release publication for gcroot/base-metatable roots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

hits=$(
  cd "$ROOT/src" && \
  rg -n "setgcref\\([^;]*(gcroot|basemt_)" \
    --glob '*.c' --glob '*.h' || true
)

if [ -n "$hits" ]; then
  printf '%s\n' "$hits"
  echo "guardrail: gcroot publications must use setgcrefroot()" >&2
  exit 1
fi

echo "M5 gcroot publication guard passed"
