#!/bin/sh
# Run focused M4 Lua-visible threading API smoke tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

if ! awk '
  /static int threading_join_core/ { infn = 1; seen = 1; next }
  infn && /lj_state_checkstack\(L, th->nresults \+ 1u\)/ { checked = 1 }
  infn && /join_actions = lj_native_leave\(L\)/ { joined_native = 1 }
  infn && /lua_State \*child = lj_thread_state_load_acq\(th\)/ { loaded = 1 }
  infn && /copyTV\(L, L->top\+\+, child->base \+ i\)/ { copied = 1 }
  infn && /threading_live_remove\(th\)/ {
    if (!checked || !loaded || !copied) bad = 1
    removed = 1
  }
  infn && /lj_safepoint_checkstop\(L, join_actions\)/ {
    if (!removed) bad = 1
    checked_stop = 1
  }
  infn && /^}/ {
    exit(seen && checked && joined_native && loaded && copied && removed &&
	 checked_stop && !bad ? 0 : 1)
  }
  END {
    if (!seen || !checked || !joined_native || !loaded || !copied ||
	!removed || !checked_stop || bad) exit 1
  }
' "$ROOT/src/lib_threading.c"; then
  echo "guardrail: join must cleanup results/live root before STOPREQ check" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-threading-api.lua"

echo "M4 threading API tests passed"
