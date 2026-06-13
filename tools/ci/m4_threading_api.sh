#!/bin/sh
# Run focused M4 Lua-visible threading API smoke tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

if ! awk '
  /static int threading_join_core/ { infn = 1; seen = 1; next }
  infn && /lj_state_checkstack\(L, th->nresults \+ 1u\)/ { checked = 1 }
  infn && /copyTV\(L, L->top\+\+, th->L->base \+ i\)/ { copied = 1 }
  infn && /threading_live_remove\(th\)/ {
    if (!checked || !copied) bad = 1
    removed = 1
  }
  infn && /^}/ { exit(seen && checked && copied && removed && !bad ? 0 : 1) }
  END { if (!seen || !checked || !copied || !removed || bad) exit 1 }
' "$ROOT/src/lib_threading.c"; then
  echo "guardrail: join must keep live root until after result stack growth/copy" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-threading-api.lua"

echo "M4 threading API tests passed"
