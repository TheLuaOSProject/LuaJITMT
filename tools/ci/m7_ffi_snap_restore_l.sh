#!/bin/sh
# Guard M7 FFI snapshot restore cdata allocation passing active lua_State.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_cdata_newx_l(L, cts, id, sz, info)' \
  'GCcdata *lj_cdata_newx_l(lua_State *L, CTState *cts'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI snapshot explicit-L marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_cdata_new\(CTState|lj_cdata_newx\(CTState|lj_cdata_new\(cts|lj_cdata_newx\(cts' \
  "$ROOT/src/lj_cdata.h" "$ROOT/src/lj_cdata.c" "$ROOT/src/lj_snap.c"; then
  echo "guardrail: cdata allocation wrappers must stay explicit-L only" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" "$ROOT/tests/t-ffi-snap-restore-l.lua"

echo "M7 FFI snapshot restore explicit-L guard passed"
