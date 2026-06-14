#!/bin/sh
# Guard M7 ffi.pin root publication and release.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
SRC="$ROOT/src/lib_ffi.c $ROOT/src/lj_ctype.h $ROOT/src/lj_gc.c $ROOT/src/lj_gc2.c $ROOT/src/lj_obj.h"

for needle in \
  'UDTYPE_FFI_PIN' \
  'GCtab *pinmt' \
  'LJLIB_MODULE_ffi_pin' \
  'LJLIB_CF(ffi_pin)' \
  'copyTVrel(L, (TValue *)uddata(ud), &nilv)' \
  'lj_udata_udtype_rel(ud, UDTYPE_FFI_PIN)' \
  'gc_marktv(g, &tv);  /* 11.6 ffi.pin() root. */' \
  'gc2_mark_tv_worker(g, &tv);  /* 11.6 ffi.pin() root. */' \
  'gc_markobj(g, cts->pinmt)' \
  'lj_gc2_markobj(g, obj2gco(cts->pinmt))'
do
  if ! rg -F -q "$needle" $SRC; then
    echo "guardrail: missing ffi.pin marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'pin_token|ffi_pin_lock|ffi_pin_unlock|GCROOT_FFI_PIN|ffi_pin.*registry|ffi_pin.*lj_tab_set' \
  $SRC; then
  echo "guardrail: ffi.pin must use the userdata-held root without a token or registry mutation" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-pin.lua" \
  "${LJ_M7_FFI_PIN_THREADS:-4}" "${LJ_M7_FFI_PIN_ITERS:-80}"

echo "M7 ffi.pin guard passed"
