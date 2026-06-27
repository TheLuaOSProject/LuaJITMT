#!/bin/sh
# Run the Lua-defined M4 VM shutdown handling test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])node[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lib_threading.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw LJThreadLive next-link access is forbidden; use lj_thread_live_next_* helpers' >&2
  exit 1
fi
if hits=$(sed -n '/static void gc_mark_threading_live/,/^}/p' \
    "$ROOT/src/lj_gc.c" | grep -nE -- 'node[[:space:]]*->[[:space:]]*next' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw LJThreadLive GC next-link traversal is forbidden; use lj_thread_live_next_acq' >&2
  exit 1
fi
if hits=$(sed -n '/static void gc2_scan_threading_live_roots/,/^}/p' \
    "$ROOT/src/lj_gc2.c" | grep -nE -- 'node[[:space:]]*->[[:space:]]*next' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw LJThreadLive GC2 next-link traversal is forbidden; use lj_thread_live_next_acq' >&2
  exit 1
fi
for helper in mt_active_acq \
  mt_active_cas \
  mt_live_acq \
  mt_live_add_rlx \
  mt_live_sub_acqrel \
  mt_live_futex_wait \
  mt_live_futex_wake \
  mt_gc_exclusive_acq \
  mt_gc_exclusive_rel \
  mt_gc_exclusive_cas \
  mt_gc_exclusive_futex_wait \
  mt_gc_exclusive_futex_wake \
  mt_shutdown_acq \
  mt_shutdown_rel; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for MT lifecycle/GC state" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*mt_(active|live|gc_exclusive|shutdown)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*mt_(active|live|gc_exclusive|shutdown)([^[:alnum:]_]|$)' \
    "$ROOT"/src/*.c \
    "$ROOT/tests/t-gc2-traverse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw MT lifecycle/GC state access is forbidden; use mt_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m4_threading_shutdown
