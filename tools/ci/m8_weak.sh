#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for helper in gc2_weak_count_acq gc2_weak_count_store_rlx \
  gc2_weak_stack_acq gc2_weak_stack_store_rlx gc2_weak_stack_rel \
  gc2_weak_ready_acq gc2_weak_ready_store_rlx gc2_weak_ready_rel \
  gc2_weak_capacity_acq gc2_weak_capacity_store_rlx \
  gc2_weak_capacity_rel \
  gc2_weak_count_add gc2_weak_scan_cursor_acq \
  gc2_weak_scan_cursor_store_rlx gc2_weak_scan_cursor_cas \
  gc2_weak_clear_cursor_acq gc2_weak_clear_cursor_store_rlx \
  gc2_weak_clear_cursor_cas; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 weak snapshot state" >&2
    exit 1
  fi
done

if hits=$(grep -nE -- '->[[:space:]]*gc2[.](weak_count|weak_scan_cursor|weak_clear_cursor)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](weak_count|weak_scan_cursor|weak_clear_cursor)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 weak snapshot state access is forbidden; use gc2_weak_* helpers' >&2
  exit 1
fi

if hits=$(grep -nE -- '->[[:space:]]*gc2[.](weak_stack|weak_ready|weak_capacity)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](weak_stack|weak_ready|weak_capacity)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 weak snapshot vector access is forbidden; use gc2_weak_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m8_weak
