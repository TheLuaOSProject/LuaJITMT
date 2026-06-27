#!/bin/sh
# Run the Lua-defined M5 threading allocator routing guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
for helper in lj_arena_owner_acq \
  lj_arena_owner_rel \
  lj_arena_alloc_owner_acq \
  lj_arena_alloc_owner_rel; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_arena.h"; then
    printf '%s\n' "${helper} helper is required for arena owner routing" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- 'hdr[.]owner_tid|alloc[.]owner_tid|->[[:space:]]*owner_tid([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_arena.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_state.c" \
    "$ROOT/src/lj_tg.c" \
    "$ROOT/src/lj_tg.h" \
    "$ROOT/src/lib_threading.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw arena owner routing access is forbidden; use lj_arena_owner_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_threading_alloc
