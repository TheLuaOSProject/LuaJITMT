#!/bin/sh
# Build and run M5 table CAS store guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-cas-store

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-cas-store.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'LJ_TAB_STORE_CAS_OK' \
  'LJ_TAB_STORE_CAS_FORWARD' \
  'lj_tab_trystoretv_cas(lua_State *L, TValue *dst, cTValue *src)' \
  'lj_tv_load_acq(&old, dst)' \
  'if (tvisforward(&old))' \
  'lj_tv_cas(dst, &old, src)' \
  'la_cpu_pause();  /* Slot became FORWARD after lookup; re-resolve it. */' \
  'lj_meta_tsettv_pair(lua_State *L, cTValue *o, cTValue *k, cTValue *v)' \
  'lj_tab_trystoretv_cas(L, dst, v) == LJ_TAB_STORE_CAS_OK' \
  'assert(lj_meta_tsettv_pair(L, &L->top[0], &key, &val) == &newarray[k])' \
  'C API settable saw FORWARD after lookup.' \
  'C API setfield saw FORWARD after lookup.' \
  'C API rawset saw FORWARD after lookup.' \
  'C API rawseti saw FORWARD after lookup.' \
  'lj_tab_trystoretv_cas(L, o, val) == LJ_TAB_STORE_CAS_OK' \
  'lj_tab_trystoretv_cas(L, dst, key+1) == LJ_TAB_STORE_CAS_OK' \
  'lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK' \
  'table_insert_shift_store(lua_State *L, GCtab *t, int32_t i)' \
  'table.insert shift saw FORWARD after lookup.' \
  'table_insert_value_store(lua_State *L, GCtab *t, int32_t i,' \
  'table.insert value saw FORWARD after lookup.' \
  'exercise_table_insert_forward_retry' \
  'exercise_capi_rawseti_forward_retry' \
  'exercise_capi_settable_forward_retry' \
  'exercise_capi_rawset_forward_retry' \
  'exercise_capi_setfield_forward_retry' \
  't-tab-cas-store OK'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/src/lj_tab.h" \
      "$ROOT/src/lj_meta.c" "$ROOT/src/lj_api.c" \
      "$ROOT/src/lib_table.c" \
      "$ROOT/tests/t-tab-cas-store.c"; then
    echo "guardrail: missing table CAS store marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /TValue \*lj_meta_tsettv_pair\(lua_State \*L,/ { inpair = 1 }
  inpair && /copyTVrel\(L, dst, v\)/ { raw = 1 }
  inpair && /for \(;;\)/ { loop = 1 }
  inpair && /meta_tset\(L, o, k, &owner\)/ { resolve = 1 }
  inpair && /lj_tab_trystoretv_cas\(L, dst, v\)/ { cas = 1 }
  inpair && /LJ_TAB_STORE_CAS_OK/ { ok = 1 }
  inpair && /lj_gc2_barrier_tv_pair\(L, owner \? obj2gco\(owner\) : NULL, v\)/ { barrier = 1 }
  inpair && /la_cpu_pause\(\);  \/\* Slot became FORWARD after lookup; re-resolve it\. \*\// { retry = 1 }
  inpair && /^}/ { inpair = 0 }
  END { exit !raw && loop && resolve && cas && ok && barrier && retry ? 0 : 1 }
' "$ROOT/src/lj_meta.c"; then
  echo "guardrail: lj_meta_tsettv_pair must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /int lj_tab_trystoretv_cas\(lua_State \*L,/ { infn = 1 }
  infn && /lj_tv_load_acq\(&old, dst\)/ { load = 1 }
  infn && /tvisforward\(&old\)/ { forward = 1 }
  infn && /lj_tv_cas\(dst, &old, src\)/ { cas = 1 }
  infn && /LJ_TAB_STORE_CAS_OK/ { ok = 1 }
  infn && /LJ_TAB_STORE_CAS_FORWARD/ { fwd = 1 }
  infn && /la_cpu_pause\(\)/ { pause = 1 }
  infn && /^}/ { infn = 0 }
  END { exit load && forward && cas && ok && fwd && pause ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table CAS helper must acquire-load, CAS, and report FORWARD" >&2
  exit 1
fi

if ! awk '
  /static void table_insert_shift_store\(lua_State \*L,/ { infn = 1 }
  infn && /lj_tab_storetv\(L, dst, &val\)/ { raw = 1 }
  infn && /lj_tab_storenil\(L, dst\)/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_tab_setint\(L, t, i\)/ { resolve = 1 }
  infn && /lj_tab_getint\(t, i-1\)/ { loadsrc = 1 }
  infn && /lj_tab_trystoretv_cas\(L, dst, &val\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /lj_gc2_barrier_weak_write\(L, t, &key, &val\)/ { weak = 1 }
  infn && /table\.insert shift saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { infn = 0 }
  END { exit !raw && loop && resolve && loadsrc && cas && weak && retry ? 0 : 1 }
' "$ROOT/src/lib_table.c"; then
  echo "guardrail: table.insert shift stores must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /static TValue \*table_insert_value_store\(lua_State \*L,/ { infn = 1 }
  infn && /copyTVrel\(L, dst, src\)/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_tab_setint\(L, t, i\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, dst, src\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /table\.insert value saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { infn = 0 }
  END { exit !raw && loop && resolve && cas && retry ? 0 : 1 }
' "$ROOT/src/lib_table.c"; then
  echo "guardrail: table.insert value stores must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(table_insert\)/ { infn = 1 }
  infn && /copyTVrel\(L, dst, L->top-1\)/ { raw = 1 }
  infn && /lj_tab_storetv\(L, dst, &val\)/ { raw = 1 }
  infn && /lj_tab_storenil\(L, dst\)/ { raw = 1 }
  infn && /table_insert_shift_store\(L, t, i\)/ { shift = 1 }
  infn && /table_insert_value_store\(L, t, i, L->top-1\)/ { value = 1 }
  infn && /lj_gc_pubtabtv\(L, t, dst\)/ { parent = 1 }
  infn && /^}/ { infn = 0 }
  END { exit !raw && shift && value && parent ? 0 : 1 }
' "$ROOT/src/lib_table.c"; then
  echo "guardrail: table.insert must route stores through CAS helpers" >&2
  exit 1
fi

if ! awk '
  /LUA_API void lua_settable\(lua_State \*L,/ { infn = 1 }
  infn && /copyTVrel\(L, o, val\)/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_meta_tset_owner\(L, t, L->top-2, &owner\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, o, val\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /lj_gc2_barrier_weak_write\(L, owner, key, val\)/ { weak = 1 }
  infn && /lj_gc2_barrier_tv_pair\(L, obj2gco\(owner\), o\)/ { parent = 1 }
  infn && /C API settable saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { infn = 0 }
  END { exit !raw && loop && resolve && cas && weak && parent && retry ? 0 : 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: lua_settable must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /LUA_API void lua_setfield\(lua_State \*L,/ { infn = 1 }
  infn && /copyTVrel\(L, o, val\)/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_meta_tset_owner\(L, t, &key, &owner\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, o, val\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /lj_gc2_barrier_weak_write\(L, owner, &key, val\)/ { weak = 1 }
  infn && /lj_gc2_barrier_tv_pair\(L, obj2gco\(owner\), o\)/ { parent = 1 }
  infn && /C API setfield saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { infn = 0 }
  END { exit !raw && loop && resolve && cas && weak && parent && retry ? 0 : 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: lua_setfield must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /LUA_API void lua_rawset\(lua_State \*L,/ { infn = 1 }
  infn && /copyTVrel\(L, dst, key\+1\)/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_tab_set\(L, t, key\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, dst, key\+1\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /lj_gc2_barrier_weak_write\(L, t, key, key\+1\)/ { weak = 1 }
  infn && /lj_gc_pubtab\(L, t\)/ { parent = 1 }
  infn && /C API rawset saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { infn = 0 }
  END { exit !raw && loop && resolve && cas && weak && parent && retry ? 0 : 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: lua_rawset must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /LUA_API void lua_rawseti\(lua_State \*L,/ { infn = 1 }
  infn && /copyTVrel\(L, dst, src\)/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_tab_setint\(L, t, n\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, dst, src\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /lj_gc2_barrier_weak_write\(L, t, &key, src\)/ { weak = 1 }
  infn && /lj_gc_pubtabtv\(L, t, dst\)/ { parent = 1 }
  infn && /C API rawseti saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { infn = 0 }
  END { exit !raw && loop && resolve && cas && weak && parent && retry ? 0 : 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: lua_rawseti must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! rg -F -q 'm5_tab_cas_store.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: table CAS store guard is not wired into M5 aggregate" >&2
  exit 1
fi

echo "M5 table CAS store tests passed"
