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
exec "$ROOT/tools/ci/lua_test.sh" m4_threading_shutdown
