#!/bin/sh
# Guard M7 FFI cdata finalizer registry concurrency bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
DUMP=${TMPDIR:-/tmp}/lj_t-ffi-gc-trace.dump

for needle in \
  'typedef struct FinRegGen' \
  'FinRegGen *fin_head' \
  'lj_ctype_fin_head(CTState *cts)' \
  'lj_ctype_fin_get(lua_State *L, CTState *cts, cTValue *key,' \
  'lj_ctype_fin_newgen(lua_State *L, CTState *cts, cTValue *key,' \
  'ctype_fin_has_claim(CTState *cts, cTValue *claim)' \
  'while (ctype_fin_has_claim(cts, claim))' \
  'lj_tab_set(L, t, key);  /* Private generation, unpublished. */' \
  '11.4 FINREG generation CAS publish.' \
  'lj_ctype_fin_istab(global_State *g, GCtab *t)' \
  'lj_ctype_fin_mark(global_State *g' \
  'lj_ctype_fin_freetabs(global_State *g, CTState *cts)' \
  'la_cas8(uint8_t *p,uint8_t *exp,uint8_t des,int mo_s,int mo_f)' \
  'lj_tv_cas(TValue *dst, TValue *expect,' \
  'lj_obj_addgcflags_atomic(GCobj *o, uint8_t flags)' \
  'lj_obj_cleargcflags_atomic(GCobj *o, uint8_t flags)' \
  'LJ_CDATA_FINCLAIM_U64' \
  'lj_cdata_fin_claim_any(TValue *tv, TValue *old)' \
  'lj_cdata_fin_claim_func(TValue *tv, TValue *old)' \
  'lj_cdata_fin_claim_func(slot, &fin)' \
  'lj_cdata_fin_storenil(L, tv)' \
  'TValue *anchor = L->top' \
  'lj_gc_barrierroot(L, &val)' \
  'L->top = anchor' \
  'Missing clear is a no-op; avoid structural insert.' \
  'lj_tab_try_newkey_anchor(lua_State *L, GCtab *t, cTValue *key,' \
  'lj_tab_try_newkey_chain(lua_State *L, GCtab *t, cTValue *key,' \
  'Another claimed empty anchor is publishing key.' \
  'Linked collision insert has not published key.' \
  '11.4 FINREG collision insert CAS-prepend.' \
  '11.4 FINREG publish after claim resolution.' \
  'while (ffi_fin && lj_cdata_fin_isclaim(&val))' \
  '#include "lj_cdata.h"' \
  'lj_ctype_fin_get(L, cts, &key, &t)' \
  'lj_ctype_fin_get(L, cts, key, &t)' \
  'lj_cdata_setfin(L, cd, gcV(tv), itype(tv))' \
  'lj_ir_call(J, IRCALL_lj_cdata_setfin, trcd, trobj,' \
  'IRCALL_lj_cdata_setfin' \
  'lj_gc2_finalizer_try_enter(global_State *g)' \
  'peer finalizer dispatch backs off'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI finalizer registry marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'fin_token|fin_claims|lj_ctype_fin_lock|lj_ctype_fin_unlock|lj_ctype_fin_claim_|lj_tab_newkey_finreg_grow|TAB_FINREG_CHAIN_RETRY_MAX' \
  "$ROOT/src"; then
  echo "guardrail: removed FINREG token/claim/grow bridge must not reappear" >&2
  exit 1
fi

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

if ! awk '
  /void lj_cdata_setfin\(lua_State \*L, GCcdata \*cd,/ { infn = 1 }
  infn && /lj_ctype_fin_head\(cts\)/ { exit(seen ? 0 : 1) }
  infn && /Missing clear is a no-op; avoid structural insert\./ { seen = 1 }
  END { if (infn) exit(seen ? 0 : 1); exit 1 }
' "$ROOT/src/lj_cdata.c"; then
  echo "guardrail: missing ffi.gc(cd, nil) clear must return before structural insertion" >&2
  exit 1
fi

if awk '
  /void lj_cdata_setfin\(lua_State \*L, GCcdata \*cd,/ { infn = 1 }
  infn && /^}/ { infn = 0 }
  infn && /GCROOT_FFI_FIN|lj_tab_set\(L, t, &key\)/ {
    bad = 1
    print
  }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_cdata.c"; then
  echo "guardrail: lj_cdata_setfin must use FINREG helpers, not GCROOT_FFI_FIN/legacy lj_tab_set" >&2
  exit 1
fi

if ! awk '
  /void lj_cdata_setfin\(lua_State \*L, GCcdata \*cd,/ { infn = 1; anchor = 0; chain = 0; newgen = 0 }
  infn && /lj_tab_try_newkey_anchor\(L, t, &key, &old, &tv\)/ { anchor = 1 }
  infn && /lj_tab_try_newkey_chain\(L, t, &key, &old, &tv\)/ { chain = 1 }
  infn && /lj_ctype_fin_newgen\(L, cts, &key, &old, &t, &tv\)/ { newgen = 1 }
  infn && /^}/ { found = anchor && chain && newgen; infn = 0 }
  END { exit found ? 0 : 1 }
' "$ROOT/src/lj_cdata.c"; then
  echo "guardrail: enabled missing-key registration must try anchor/chain and FINREG generation publish before returning" >&2
  exit 1
fi

if ! awk '
  /int lj_ctype_fin_newgen\(lua_State \*L,/ { infn = 1; wait = 0; scan = 0; private = 0; cas = 0 }
  infn && /while \(ctype_fin_has_claim\(cts, claim\)\)/ { wait = 1 }
  infn && /ctype_fin_any_key\(cts, L, key\)/ { scan = 1 }
  infn && /lj_tab_set\(L, t, key\).*Private generation/ { private = 1 }
  infn && /la_casptr\(\(void \*\*\)&cts->fin_head/ { cas = 1 }
  infn && /^}/ { found = wait && scan && private && cas; infn = 0 }
  END { exit found ? 0 : 1 }
' "$ROOT/src/lj_ctype.c"; then
  echo "guardrail: FINREG generation publisher must wait claims, scan keys, and CAS-publish" >&2
  exit 1
fi

if awk '
  /lj_tab_try_newkey_anchor\(L, t, &key, &old, &tv\)|lj_tab_try_newkey_chain\(L, t, &key, &old, &tv\)/ {
    if (FILENAME !~ /src\/lj_cdata\.c$/) {
      bad = 1
      print FILENAME ":" FNR ":" $0
    }
  }
  END { exit bad ? 0 : 1 }
' "$ROOT"/src/*.c; then
  echo "guardrail: lock-free table claim helpers are currently only validated for GCROOT_FFI_FIN" >&2
  exit 1
fi

if ! awk '
  /void lj_cdata_setfin\(lua_State \*L, GCcdata \*cd,/ { infn = 1; got = 0 }
  infn && /lj_ctype_fin_get\(L, cts, &key, &t\)/ { got = 1 }
  infn && got && /lj_cdata_fin_claim_any\(tv, &old\)/ { found = 1; infn = 0 }
  END { exit found ? 0 : 1 }
' "$ROOT/src/lj_cdata.c"; then
  echo "guardrail: existing FINREG slot updates must claim the discovered slot" >&2
  exit 1
fi

if awk '
  /int lj_tab_try_newkey_anchor\(lua_State \*L,/ { infn = 1 }
  /int lj_tab_try_newkey_chain\(lua_State \*L,/ { infn = 1 }
  infn && /lj_gc_pubtab\(L, t\)|lj_gc2_barrier_weak_key\(L, t, key\)/ {
    bad = 1
    print
  }
  infn && /^}/ { infn = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: FINREG claim helpers must not publish the table before claim resolution" >&2
  exit 1
fi

if awk '
  /int lj_tab_try_newkey_chain\(lua_State \*L,/ { infn = 1 }
  infn && /lj_tab_newkey\(L, t|lj_tab_set\(L, t|rehashtab\(L, t|lj_tab_resize|setfreetop\(/ {
    bad = 1
    print
  }
  infn && /^}/ { infn = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: FINREG collision helper must stay within the current hash generation" >&2
  exit 1
fi

if awk '
  /int lj_ctype_fin_newgen\(lua_State \*L,/ { infn = 1 }
  infn && /lj_tab_newkey\(L, t|rehashtab\(L, t|lj_tab_resize\(L, t/ {
    bad = 1
    print
  }
  infn && /^}/ { infn = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_ctype.c"; then
  echo "guardrail: FINREG generation helper must not resize an existing table" >&2
  exit 1
fi

if ! awk '
  /static int gc_finalize_cdata_slot_owned\(lua_State \*L,/ { infn = 1; get = 0; claim = 0 }
  infn && /lj_ctype_fin_get\(L, cts, key, &t\)/ { get = 1 }
  infn && /lj_cdata_fin_claim_func\(slot, &fin\)/ { claim = 1 }
  infn && /^}/ { found = get && claim; infn = 0 }
  END { exit found ? 0 : 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: cdata finalizer slot claims must scan FINREG generations and claim the slot" >&2
  exit 1
fi

if ! awk '
  /static void cdata_fin_store\(lua_State \*L,/ { infn = 1; copied = 0; weak = 0 }
  infn && /copyTVrel\(L, tv, val\)/ { copied = 1 }
  infn && copied && /lj_gc2_barrier_weak_key\(L, t, &key\)/ { weak = 1 }
  infn && /lj_gc_pubtab\(L, t\).*FINREG publish after claim resolution/ {
    found = copied && weak
    infn = 0
  }
  END { exit found ? 0 : 1 }
' "$ROOT/src/lj_cdata.c"; then
  echo "guardrail: FINREG table publication must happen after finalizer claim resolution" >&2
  exit 1
fi

for file in "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; do
  if ! awk '
    /static int gc2_traverse_tab\(global_State \*g, GCtab \*t\)/ { infn = 1 }
    /static int gc_traverse_tab\(global_State \*g, GCtab \*t\)/ { infn = 1 }
    infn && /while \(ffi_fin && lj_cdata_fin_isclaim\(&val\)\)/ { found = 1 }
    infn && /^}/ { infn = 0 }
    END { exit found ? 0 : 1 }
  ' "$file"; then
    echo "guardrail: GC traversal of every FINREG generation must wait out claim sentinels in $file" >&2
    exit 1
  fi
  if ! rg -F -q 'lj_ctype_fin_istab(g, t)' "$file"; then
    echo "guardrail: GC traversal must identify every FINREG generation in $file" >&2
    exit 1
  fi
  if ! rg -F -q 'lj_ctype_fin_mark(g,' "$file"; then
    echo "guardrail: GC roots must mark every FINREG generation in $file" >&2
    exit 1
  fi
done

if ! awk '
  /int lj_gc_cdata_fin_pending\(global_State \*g\)/ { inpending = 1; pendinghead = 0; pendingscan = 0 }
  inpending && /cts->fin_head/ { pendinghead = 1 }
  inpending && /gc_cdata_fin_pending_tab\(t\)/ { pendingscan = 1 }
  inpending && /^}/ { pending = pendinghead && pendingscan; inpending = 0 }
  /void lj_gc_finalize_cdata\(lua_State \*L\)/ { indrain = 1; separate = 0 }
  indrain && /gc_separate_cdata_finalizers\(g\)/ { separate = 1 }
  indrain && /^}/ { drain = separate; indrain = 0 }
  /void lj_gc_finalize_cdata_disable\(global_State \*g\)/ { indisable = 1; disablehead = 0; disable = 0 }
  indisable && /cts->fin_head/ { disablehead = 1 }
  indisable && /setgcrefnull\(t->metatable\).*FINREG generations disabled/ { disable = 1 }
  indisable && /^}/ { disabled = disablehead && disable; indisable = 0 }
  END { exit (pending && drain && disabled) ? 0 : 1 }
' "$ROOT/src/lj_gc.c"; then
  echo "guardrail: close-time cdata finalizer drain must queue cdata and scan/disable every FINREG generation" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(ffi_gc\)/ { infn = 1 }
  infn && /lj_cdata_setfin\(L, cd, gcval\(fin\), itype\(fin\)\)/ { seen = 1 }
  infn && /return 1;/ { found = seen; infn = 0 }
  END { exit found ? 0 : 1 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: ffi.gc must route finalizer changes through lj_cdata_setfin" >&2
  exit 1
fi

if awk '
  /static void crec_finalizer\(jit_State \*J,/ { infn = 1 }
  infn && /lj_trace_err_info\(J, LJ_TRERR_NYIFFU\)/ {
    bad = 1
    print
  }
  infn && /^}/ { infn = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_crecord.c"; then
  echo "guardrail: ffi.gc recorder must emit FINREG mutation, not NYI" >&2
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

"$ROOT/src/luajit" "$ROOT/tests/t-ffi-gc-finreg.lua" \
  "${LJ_M7_FFI_FIN_THREADS:-6}" "${LJ_M7_FFI_FIN_ITERS:-240}"

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" "$ROOT/tests/t-ffi-gc-trace.lua"

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir "$ROOT/tests/t-ffi-gc-trace.lua" \
  >"$DUMP"

if ! rg -q 'lj_cdata_setfin' "$DUMP"; then
  echo "guardrail: traced ffi.gc dump must emit lj_cdata_setfin" >&2
  exit 1
fi

echo "M7 FFI finalizer registry guard passed"
