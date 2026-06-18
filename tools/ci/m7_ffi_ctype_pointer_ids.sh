#!/bin/sh
# Guard M7 FFI ctype raw-ID use at API pointer-stability boundaries.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'ctype_rawid(CTState *cts, CTypeID id)' \
  'ctype_rawrefid(CTState *cts, CTypeID id)' \
  'return ctype_get(cts, ctype_rawid(cts, id))' \
  'return ctype_get(cts, ctype_rawrefid(cts, id))' \
  'lj_cdata_index_l(lua_State *L, CTState *cts, GCcdata *cd' \
  'CTypeID *idp' \
  'ctype_rawchildid(CTState *cts, CType *ct)' \
  'static int ffi_index_meta(lua_State *L, CTState *cts, CTypeID id' \
  'lj_cdata_index_l(L, cts, cdataV(o), o+1, &p, &qual, &id)' \
  'CTypeID id = ctype_rawid(cts, cd->ctypeid)' \
  'lj_cconv_multi_init(CTState *cts, CTypeID did' \
  'ctype_rawrefid(cts, cdataV(o)->ctypeid) == did' \
  'lj_cconv_ct_init_l(L, cts, ct, rid' \
  'lj_cconv_ct_ct_l(lua_State *L, CTState *cts, CType *d,' \
  'CTypeID did, CType *s, CTypeID sid' \
  'cconv_err_conv_l(L, cts, did, sid, s, flags)' \
  'lj_cconv_ct_tv_l(lua_State *L, CTState *cts, CType *d,' \
  'CTypeID did, uint8_t *dp' \
  'd = ctype_get(cts, did);  /* cts->tab may have been reallocated. */' \
  'lj_cdata_set_l(L, cts, ct, id' \
  'lj_cconv_ct_tv_l(L, cts, d, did' \
  'lj_cconv_ct_tv_l(L, cts, ctr, rid' \
  'crec_index_meta(jit_State *J, CTState *cts, CTypeID id' \
  'crec_index_meta(J, cts, id, rd)' \
  'CTypeID sid[2]' \
  'CTypeID id0 = i ? sid[0] : 0' \
  'lj_ir_kint(J, id)' \
  'lj_ctype_info(cts, id, &sz)' \
  'return lj_cconv_tv_ct_l(L, cts, ctr, rid' \
  'gcsteps += lj_cconv_tv_ct_l(L, cts, cta, aid' \
  'CTypeID rid1 = ctype_rawrefid(cts, id1)' \
  'CTypeID rid2 = ctype_rawrefid(cts, id2)' \
  'if (rid1 == rid2)' \
  'rid1 == ctype_rawid(cts, ctype_cid(ct2->info))' \
  'lj_ctype_metatv(cts, &metatv, rid, MM_tostring)' \
  'lj_ctype_setmeta(cts, rid, mt)' \
  'ctype_preptype(CTRepr *ctr, CTypeID id' \
  'ctype_prepnum(ctr, id)' \
  'cp_err_badidx(CPState *cp, CTypeID id)' \
  'id = ctype_rawrefid(cp->cts, k->id)' \
  'GCstr *s = lj_ctype_repr(cp->L, id, NULL)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI ctype raw-ID marker: $needle" >&2
    exit 1
  fi
done

if awk '
  /static int ffi_index_meta/ { inside = 1 }
  inside && /LJLIB_CF\(ffi_meta___index\)/ { inside = 0 }
  /LJLIB_CF\(ffi_meta___tostring\)/ { inside = 1 }
  inside && /LJLIB_CF\(ffi_clib___index\)/ { inside = 0 }
  /LJLIB_CF\(ffi_istype\)/ { inside = 1 }
  inside && /LJLIB_CF\(ffi_sizeof\)/ { inside = 0 }
  /LJLIB_CF\(ffi_metatype\)/ { inside = 1 }
  inside && /LJLIB_CF\(ffi_gc\)/ { inside = 0 }
  inside && /ctype_typeid\(cts, ct\)/ { print; bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: FFI API raw-ID paths must not derive IDs from CType *" >&2
  exit 1
fi

if awk '
  /void lj_cconv_ct_tv_l\(lua_State \*L, CTState \*cts, CType \*d,/ { inside = 1 }
  inside && /^}/ { inside = 0 }
  inside && /ctype_typeid\(cts, d\)/ { print; bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_cconv.c"; then
  echo "guardrail: TValue-to-C conversion must not derive destination IDs from CType *" >&2
  exit 1
fi

if rg -n 'ctype_typeid\(cts' "$ROOT/src/lj_cconv.c" "$ROOT/src/lj_carith.c" \
    "$ROOT/src/lj_cdata.c" "$ROOT/src/lj_crecord.c"; then
  echo "guardrail: runtime conversion paths must not derive IDs from CType *" >&2
  exit 1
fi

if rg -n 'ctype_typeid\(' "$ROOT/src/lj_ctype.c" "$ROOT/src/lj_cparse.c" \
    "$ROOT/src/lj_crecord.c" "$ROOT/src/lj_cconv.c" \
    "$ROOT/src/lj_carith.c" "$ROOT/src/lj_cdata.c" "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: x86_64 FFI raw-ID paths must not derive IDs from CType *" >&2
  exit 1
fi

if rg -n 'ctype_typeid\(' "$ROOT/src"; then
  echo "guardrail: ctype table readers must not derive IDs from CType *" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-ctype-pointer-ids.lua"

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" XCFLAGS="-DLUAJIT_CTYPE_CHECK_ANCHOR" >/dev/null
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-ctype-pointer-ids.lua"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdata-set-l.lua" \
  1 "${LJ_M7_FFI_SET_ITERS:-80}"

echo "M7 FFI ctype raw-ID guard passed"
