#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(ret|tail|mcret)[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_mcode.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/tests/t-jit-mcode-retire.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw MCodeRetire next-link access is forbidden; use mcode_retired_next_* helpers' >&2
  exit 1
fi
for helper in mcode_retired_head_acq mcode_retired_head_cas \
    mcode_retired_head_xchg_acqrel; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]|^[[:space:]]*${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_jit.h"; then
    printf 'required mcode retired head helper missing: %s\n' "$helper" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- 'J->[[:space:]]*retiredmcode|&J->[[:space:]]*retiredmcode' \
    "$ROOT/src/lj_mcode.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/tests/t-jit-mcode-retire.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw mcode retired head access is forbidden; use mcode_retired_head_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '\(\(MCLink \*\)[^)]*\)->[[:space:]]*next|rwlink[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_mcode.c" \
    "$ROOT/src/lj_mcode.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw MCLink next-link access is forbidden; use mcode_area_next_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_mcode_publish
