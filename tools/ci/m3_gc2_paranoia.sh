#!/bin/sh
# Run the Lua-defined M3 GC2 paranoia guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for helper in gc2_marks_this_round_acq gc2_marks_this_round_store_rlx \
  gc2_marks_this_round_add gc2_marks_this_round_xchg_acqrel; do
  if ! grep -q "static LJ_AINLINE .* ${helper}" "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 fixpoint progress" >&2
    exit 1
  fi
done

if hits=$(grep -nE -- '->[[:space:]]*gc2[.]marks_this_round|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]marks_this_round' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 fixpoint progress access is forbidden; use gc2_marks_this_round_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m3_gc2_paranoia
