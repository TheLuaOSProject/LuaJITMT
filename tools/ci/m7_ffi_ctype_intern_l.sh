#!/bin/sh
# Guard M7 FFI ctype allocation/interning passing active lua_State explicitly.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_ctype_new_l(lua_State *L, CTState *cts' \
  'lj_ctype_intern_l(lua_State *L, CTState *cts' \
  'lj_ccall_ctid_vararg(lua_State *L, CTState *cts' \
  'lj_ctype_new_l(cp->L, cp->cts' \
  'lj_ctype_intern_l(cp->L, cp->cts' \
  'lj_ctype_intern_l(L, cts' \
  'lj_ctype_intern_l(J->L, cts' \
  'lj_ccall_ctid_vararg(L, cts' \
  'lj_ccall_ctid_vararg(J->L, cts'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI ctype explicit-L marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'cts->L|parse_L|lj_ctype_new\(|lj_ctype_intern\(' "$ROOT/src"; then
  echo "guardrail: ctype allocation/interning must not route through CTState L" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdef-token.lua" \
  "${LJ_M7_FFI_CDEF_THREADS:-6}" "${LJ_M7_FFI_CDEF_ITERS:-120}"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdata-set-l.lua" \
  "${LJ_M7_FFI_SET_THREADS:-6}" "${LJ_M7_FFI_SET_ITERS:-320}"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-carith-l.lua"

echo "M7 FFI ctype explicit-L intern guard passed"
