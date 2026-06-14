#!/bin/sh
# Guard M7 FFI cdata read paths passing the active lua_State explicitly.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_cdata_newref_l(lua_State *L, CTState *cts' \
  'lj_cdata_index_l(lua_State *L, CTState *cts' \
  'lj_cdata_get_l(lua_State *L, CTState *cts' \
  'lj_cconv_tv_ct_l(lua_State *L, CTState *cts' \
  'lj_cconv_tv_bf_l(lua_State *L, CTState *cts' \
  'L2TG(L)->tmptv2' \
  'lj_cdata_index_l(L, cts' \
  'lj_cdata_get_l(L, cts' \
  'lj_cconv_tv_ct_l(L, cts, ct, sid' \
  'lj_cconv_tv_ct_l(L, cts, ctr' \
  'lj_cconv_tv_ct_l(L, cts, cta'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI explicit-L cdata read marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_cconv_tv_ct\(cts' \
  "$ROOT/src/lib_ffi.c" "$ROOT/src/lj_ccall.c" "$ROOT/src/lj_ccallback.c"; then
  echo "guardrail: active-L FFI conversion call site still uses cts->L wrapper" >&2
  exit 1
fi

if rg -n 'lj_cdata_newref\(CTState|lj_cdata_index\(CTState|lj_cdata_get\(CTState|lj_cconv_tv_ct\(CTState|lj_cconv_tv_bf\(CTState' "$ROOT/src"; then
  echo "guardrail: read-side FFI conversion wrappers must stay explicit-L only" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdata-get-l.lua" \
  "${LJ_M7_FFI_GET_THREADS:-6}" "${LJ_M7_FFI_GET_ITERS:-400}"

echo "M7 FFI cdata explicit-L read guard passed"
