#!/bin/sh
# Run the Lua-defined M4 threading API smoke test.
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
if ! awk '
  /^static uint32_t threading_join_claim_results\(lua_State \*L, lua_State \*child,/ {
    in_fn = 1
  }
  in_fn && /lj_thr_sleep_ns\(L,[[:space:]]*1000000\)/ { slept = 1 }
  in_fn && /lj_safepoint_checkstop\(L,[[:space:]]*actions\)/ { checked = 1 }
  in_fn && /threading_checkstop_fresh\(L,[[:space:]]*actions,[[:space:]]*had_stopreq\)/ { checked = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(slept && checked ? 0 : 1) }
' "$ROOT/src/lib_threading.c"; then
  printf '%s\n' 'DONE child join result waits must check STOPREQ after each native sleep slice' >&2
  exit 1
fi
if hits=$(grep -nF 'la_futex_wait(&m->state, LJ_MUTEX_LOCKED, -1)' \
    "$ROOT/src/lib_threading.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'mutex lock waits must use bounded native waits so STOPREQ is delivered before unlock' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m4_threading_api
