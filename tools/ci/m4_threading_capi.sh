#!/bin/sh
# Run the Lua-defined focused M4 public C threading API test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(awk '
  /while[[:space:]]*\(!lj_state_claim\(child, tid\)\)/ {
    line = $0
    if ((getline nextline) > 0 && nextline ~ /la_cpu_pause[[:space:]]*\(/)
      print FNR-1 ":" line "\n" FNR ":" nextline
  }
' "$ROOT/src/lib_threading.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'DONE child join result claims must wait in native slices, not spin on la_cpu_pause()' >&2
  exit 1
fi
if hits=$(grep -nF 'la_futex_wait(&m->state, LJ_MUTEX_LOCKED, -1)' \
    "$ROOT/src/lib_threading.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'mutex lock waits must use bounded native waits so STOPREQ is delivered before unlock' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m4_threading_capi
