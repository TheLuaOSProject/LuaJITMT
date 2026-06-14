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
  'la_cas8(uint8_t *p,uint8_t *exp,uint8_t des,int mo_s,int mo_f)' \
  'lj_tv_cas(TValue *dst, TValue *expect,' \
  'lj_obj_addgcflags_atomic(GCobj *o, uint8_t flags)' \
  'lj_obj_cleargcflags_atomic(GCobj *o, uint8_t flags)' \
  'LJ_CDATA_FINCLAIM_U64' \
  'lj_cdata_fin_claim_any(TValue *tv, TValue *old)' \
  'lj_cdata_fin_claim_func(TValue *tv, TValue *old)' \
  'lj_cdata_fin_claim_func(tv, &tmp)' \
  'lj_cdata_fin_storenil(L, tv)' \
  'lj_tab_get(L, t, &tmp)' \
  'lj_cdata_setfin(L, cd, gcV(tv), itype(tv))' \
  'lj_gc2_finalizer_try_enter(global_State *g)' \
  'peer finalizer dispatch backs off'
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

if awk '
  /if \(o->gch.gct == ~LJ_TCDATA\)/ { incdata = 1 }
  incdata && /lj_gc2_finalizer_leave\(g\);/ { incdata = 0 }
  incdata && /lj_ctype_fin_lock\(cts\)|lj_ctype_fin_unlock\(cts\)/ {
    bad = 1
    print
  }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: per-cdata finalizer claim must use FINREG slot CAS, not fin_token" >&2
  exit 1
fi

if awk '
  /void lj_gc_finalize_cdata\(lua_State \*L\)/ { infn = 1 }
  infn && /^}/ { infn = 0 }
  infn && /lj_ctype_fin_lock\(cts\)|lj_ctype_fin_unlock\(cts\)/ {
    bad = 1
    print
  }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: close-time cdata finalizer drain must disable FINREG without fin_token" >&2
  exit 1
fi

if rg -n 'finalizer_token|gc_finalizer_vm_lock|gc_finalizer_vm_unlock' \
  "$ROOT/src"; then
  echo "guardrail: finalizer callbacks must use the GC2 owner claim, not a shared VM-thread token" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-gc-finreg.lua" \
  "${LJ_M7_FFI_FIN_THREADS:-6}" "${LJ_M7_FFI_FIN_ITERS:-240}"

echo "M7 FFI finalizer registry guard passed"
