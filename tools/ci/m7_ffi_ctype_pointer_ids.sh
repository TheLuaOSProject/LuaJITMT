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
  'static int ffi_index_meta(lua_State *L, CTState *cts, CTypeID id' \
  'lj_cdata_index_l(L, cts, cdataV(o), o+1, &p, &qual, &id)' \
  'CTypeID id = ctype_rawid(cts, cd->ctypeid)' \
  'lj_cconv_multi_init(CTState *cts, CTypeID did' \
  'ctype_rawrefid(cts, cdataV(o)->ctypeid) == did' \
  'lj_cconv_ct_init_l(L, cts, ct, ctype_rawid(cts, id)' \
  'return lj_cconv_tv_ct_l(L, cts, ctr, rid' \
  'gcsteps += lj_cconv_tv_ct_l(L, cts, cta, aid' \
  'CTypeID rid1 = ctype_rawrefid(cts, id1)' \
  'CTypeID rid2 = ctype_rawrefid(cts, id2)' \
  'if (rid1 == rid2)' \
  'rid1 == ctype_rawid(cts, ctype_cid(ct2->info))' \
  'lj_ctype_metatv(cts, &metatv, rid, MM_tostring)' \
  'tv = lj_tab_setinth(L, t, -(int32_t)rid)'
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

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-ctype-pointer-ids.lua"

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" XCFLAGS="-DLUAJIT_CTYPE_CHECK_ANCHOR" >/dev/null
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-ctype-pointer-ids.lua"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdata-set-l.lua" \
  1 "${LJ_M7_FFI_SET_ITERS:-80}"

echo "M7 FFI ctype raw-ID guard passed"
