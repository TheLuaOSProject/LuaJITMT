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
  't-tab-cas-store OK'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/src/lj_tab.h" \
      "$ROOT/src/lj_meta.c" "$ROOT/tests/t-tab-cas-store.c"; then
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

if ! rg -F -q 'm5_tab_cas_store.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: table CAS store guard is not wired into M5 aggregate" >&2
  exit 1
fi

echo "M5 table CAS store tests passed"
