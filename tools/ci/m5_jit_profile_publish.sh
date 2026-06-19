#!/bin/sh
# Guard M5 jit.profile registry publication stores.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

hits=$(rg -n "lj_gc_(objbarrier|objbarriert|anybarriert|barrieruv|barriert|barrier)\\b" \
  "$ROOT/src/lib_jit.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: lib_jit must use M5 publication wrappers:" >&2
  echo "$hits" >&2
  exit 1
fi

if ! awk '
  /static TValue \*jit_profile_registry_store\(lua_State \*L,/ { infn = 1; next }
  infn && /copyTVrel\(L, dst, tv\)/ { raw = 1 }
  infn && /lj_tab_storenil\(L, dst\)/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_tab_set\(L, registry, key\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, dst, tv\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /jit\.profile registry saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { exit(!raw && loop && resolve && cas && retry ? 0 : 1) }
  END { if (raw || !loop || !resolve || !cas || !retry) exit 1 }
' "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.profile registry helper must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(jit_profile_start\)/ { infn = 1; next }
  infn && /copyTVrel\(L, lj_tab_set\(L, registry, &key\), &tv\)/ { raw = 1 }
  infn && /jit_profile_registry_store\(L, registry, &key, &tv\)/ { cas++ }
  infn && /lj_gc2_barrier_weak_write\(L, registry, &key, &tv\)/ { weak++ }
  infn && /lj_gc_pubtab\(L, registry\)/ { pub = 1 }
  infn && /^}/ { exit(!raw && cas >= 2 && weak >= 2 && pub ? 0 : 1) }
  END { if (raw || cas < 2 || weak < 2 || !pub) exit 1 }
' "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.profile start must CAS-publish registry anchors" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(jit_profile_stop\)/ { infn = 1; next }
  infn && /lj_tab_storenil\(L, lj_tab_set\(L, registry, &key\)\)/ { raw = 1 }
  infn && /jit_profile_registry_store\(L, registry, &key, niltv\(L\)\)/ { cas++ }
  infn && /lj_gc_pubtab\(L, registry\)/ { pub = 1 }
  infn && /^}/ { exit(!raw && cas >= 2 && pub ? 0 : 1) }
  END { if (raw || cas < 2 || !pub) exit 1 }
' "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.profile stop must CAS-publish registry clears" >&2
  exit 1
fi

echo "M5 jit.profile publication guard passed"
