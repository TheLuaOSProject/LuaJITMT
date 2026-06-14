#!/bin/sh
# Guard M7 FFI arithmetic/raw C-to-C conversion explicit-L bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_cconv_ct_ct_l(lua_State *L, CTState *cts, CType *d,' \
  'CTypeID did, CType *s, CTypeID sid' \
  'cconv_err_conv_l(lua_State *L, CTState *cts,' \
  'lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_INT_PSZ), CTID_INT_PSZ' \
  'lj_cconv_ct_ct_l(L, cts, ct, id, ca->ct[0], ca->id[0]' \
  'lj_cconv_ct_ct_l(L, cts, ct, id, ca->ct[1], ca->id[1]' \
  'lj_cconv_ct_ct_l(L, cts, ctype_get(cts, *id), *id, s, sid' \
  'lj_cdata_new_l(L, cts, id, CTSIZE_PTR)' \
  'lj_cdata_new_l(L, cts, id, 8)' \
  'CTypeID id[2]' \
  'CTypeID id0 = i ? ca->id[0] : 0' \
  'repr[i] = strdata(lj_ctype_repr(L, ca->id[i], NULL))' \
  'lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_INT32), CTID_INT32' \
  'lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_DOUBLE), CTID_DOUBLE'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_carith.c" "$ROOT/src/lj_cconv.c" \
      "$ROOT/src/lj_cconv.h"; then
    echo "guardrail: missing FFI arithmetic explicit-L marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'LJ_FUNC void lj_cconv_ct_ct\(|void lj_cconv_ct_ct\(|lj_cconv_ct_ct\(cts' \
    "$ROOT/src/lj_cconv.h" "$ROOT/src/lj_cconv.c" "$ROOT/src/lj_carith.c"; then
  echo "guardrail: legacy lj_cconv_ct_ct wrapper must stay removed" >&2
  exit 1
fi

if rg -n 'ctype_typeid\(cts' "$ROOT/src/lj_cconv.c" "$ROOT/src/lj_carith.c" \
    "$ROOT/src/lj_cdata.c"; then
  echo "guardrail: runtime C-to-C conversion paths must carry CTypeIDs" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-carith-l.lua"

echo "M7 FFI arithmetic explicit-L guard passed"
