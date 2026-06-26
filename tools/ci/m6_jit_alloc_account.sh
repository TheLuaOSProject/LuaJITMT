#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RIn -- 'gcnext(' "$ROOT/src" "$ROOT/tests" | \
    grep -vE -- '/src/lj_obj[.]h:[0-9]+:#define gcnext' || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw gcnext() traversal is forbidden; use lj_obj_gcw_acq()' >&2
  exit 1
fi
for helper in lj_tg_local_total_xchg_acqrel lj_tg_local_total_add_rlx
do
  if ! grep -q "$helper" "$ROOT/src/lj_tg.h"; then
    printf '%s\n' "missing TG allocation counter helper: $helper" >&2
    exit 1
  fi
done
if hits=$(grep -RInE -- '->[[:space:]]*local_total([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*local_total([^[:alnum:]_]|$)' \
    "$ROOT/src"/lj_*.c "$ROOT/src"/lib_*.c "$ROOT/src"/lj_*.h 2>/dev/null | \
    grep -vF "$ROOT/src/lj_tg.h:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TG allocation counter access is forbidden; use lj_tg_local_total_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_jit_alloc_account
