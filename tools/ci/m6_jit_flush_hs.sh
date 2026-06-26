#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
for helper in lj_tg_jit_exitcode_acq lj_tg_jit_exitcode_rel
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG JIT exit-code helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*jit_exitcode([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*jit_exitcode([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_err.c" "$ROOT/src/lj_trace.c" "$ROOT/src/lj_tg.h" 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG JIT exit-code access is forbidden; use lj_tg_jit_exitcode_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_flush_hs
