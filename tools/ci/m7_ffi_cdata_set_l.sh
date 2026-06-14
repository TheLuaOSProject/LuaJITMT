#!/bin/sh
# Guard M7 FFI write paths passing the active lua_State explicitly.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_cconv_ct_ct_l(lua_State *L, CTState *cts, CType *d,' \
  'CTypeID did, CType *s, CTypeID sid' \
  'lj_cconv_ct_tv_l(lua_State *L, CTState *cts, CType *d,' \
  'CTypeID did, uint8_t *dp' \
  'lj_cconv_bf_tv_l(lua_State *L, CTState *cts' \
  'lj_cconv_ct_init_l(lua_State *L, CTState *cts' \
  'lj_cdata_set_l(lua_State *L, CTState *cts, CType *d, CTypeID did' \
  'cconv_err_convtv_l(lua_State *L' \
  'cconv_err_initov_l(lua_State *L' \
  'lj_cdata_set_l(L, cts, ct, id' \
  'lj_cconv_ct_init_l(L, cts' \
  'lj_cconv_ct_tv_l(L, cts, d, did' \
  'lj_cconv_bf_tv_l(L, cts' \
  'lj_cconv_ct_tv_l(L, cts, ctr, rid' \
  'ccall_struct_arg(cc, L, cts, d, did'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI explicit-L cdata write marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_cconv_ct_tv\(CTState|lj_cconv_bf_tv\(CTState|lj_cconv_ct_init\(CTState|lj_cdata_set\(CTState' "$ROOT/src"; then
  echo "guardrail: write-side FFI conversion wrappers must stay explicit-L only" >&2
  exit 1
fi

if rg -n 'lj_cconv_ct_tv\(cts|lj_cconv_bf_tv\(cts|lj_cconv_ct_init\(cts|lj_cdata_set\(cts' \
  "$ROOT/src/lib_ffi.c" "$ROOT/src/lib_base.c" "$ROOT/src/lib_buffer.c" \
  "$ROOT/src/lib_bit.c" "$ROOT/src/lj_ccall.c" "$ROOT/src/lj_ccallback.c" \
  "$ROOT/src/lj_cdata.c" "$ROOT/src/lj_cconv.c"; then
  echo "guardrail: active-L FFI write call site still uses cts->L wrapper" >&2
  exit 1
fi

if awk '
  /void lj_cconv_ct_tv_l\(lua_State \*L, CTState \*cts, CType \*d,/ { inside = 1 }
  inside && /^}/ { inside = 0 }
  inside && /ctype_typeid\(cts, d\)/ { print; bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_cconv.c"; then
  echo "guardrail: TValue-to-C conversion must carry destination CTypeID explicitly" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdata-set-l.lua" \
  "${LJ_M7_FFI_SET_THREADS:-6}" "${LJ_M7_FFI_SET_ITERS:-320}"

echo "M7 FFI cdata explicit-L write guard passed"
