#!/bin/sh
# Guard M7 FFI arithmetic/raw C-to-C conversion explicit-L bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_INT_PSZ)' \
  'lj_cconv_ct_ct_l(L, cts, ct, ca->ct[0]' \
  'lj_cconv_ct_ct_l(L, cts, ct, ca->ct[1]' \
  'lj_cconv_ct_ct_l(L, cts, ctype_get(cts, *id)' \
  'lj_cdata_new_l(L, cts, id, CTSIZE_PTR)' \
  'lj_cdata_new_l(L, cts, id, 8)' \
  'lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_INT32)' \
  'lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_DOUBLE)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_carith.c" "$ROOT/src/lj_cconv.c"; then
    echo "guardrail: missing FFI arithmetic explicit-L marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'LJ_FUNC void lj_cconv_ct_ct\(|void lj_cconv_ct_ct\(|lj_cconv_ct_ct\(cts' \
    "$ROOT/src/lj_cconv.h" "$ROOT/src/lj_cconv.c" "$ROOT/src/lj_carith.c"; then
  echo "guardrail: legacy lj_cconv_ct_ct wrapper must stay removed" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-carith-l.lua"

echo "M7 FFI arithmetic explicit-L guard passed"
