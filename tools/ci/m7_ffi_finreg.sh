#!/bin/sh
# Guard M7 FFI cdata finalizer registry concurrency bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'uint32_t fin_token' \
  'lj_ctype_fin_lock(CTState *cts)' \
  'la_cas32(&cts->fin_token, &expect, 1, LA_ACQ_REL, LA_ACQ)' \
  'la_futex_wait(&cts->fin_token, 1, 1000000)' \
  'lj_ctype_fin_unlock(CTState *cts)' \
  'lj_ctype_fin_lock(cts)' \
  'lj_tab_get(L, t, &tmp)' \
  'lj_cdata_setfin(L, cd, gcV(tv), itype(tv))'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI finalizer registry marker: $needle" >&2
    exit 1
  fi
done

if awk '
  /LJLIB_CF\(ffi_new\)/ { innew = 1 }
  innew && /LJLIB_CF\(ffi_cast\)/ { innew = 0 }
  innew && /GCROOT_FFI_FIN|lj_tab_set\(L, t, o-1\)|lj_obj_addgcflags\(obj2gco\(cd\), LJ_GC_CDATA_FIN\)/ {
    bad = 1
    print
  }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: ffi.new ctype __gc registration must route through lj_cdata_setfin" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-gc-finreg.lua" \
  "${LJ_M7_FFI_FIN_THREADS:-6}" "${LJ_M7_FFI_FIN_ITERS:-240}"

echo "M7 FFI finalizer registry guard passed"
