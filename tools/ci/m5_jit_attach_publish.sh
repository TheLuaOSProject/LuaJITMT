#!/bin/sh
# Guard M5 jit.attach event table publication stores.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if ! awk '
  /static TValue \*jit_attach_event_store\(lua_State \*L,/ { infn = 1; next }
  infn && /lj_tab_storenil\(L, dst\)/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_tab_set\(L, tab, key\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, dst, src\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /jit\.attach event table saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { exit(!raw && loop && resolve && cas && retry ? 0 : 1) }
  END { if (raw || !loop || !resolve || !cas || !retry) exit 1 }
' "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.attach event helper must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(jit_attach\)/ { infn = 1; next }
  infn && /lj_tab_storenil\(L, lj_tab_set\(L, tabV\(L->top-2\), L->top-1\)\)/ { raw = 1 }
  infn && /jit_attach_event_store\(L, tabV\(L->top-2\), L->top-1, niltv\(L\)\)/ { cas = 1 }
  infn && /^}/ { exit(!raw && cas ? 0 : 1) }
  END { if (raw || !cas) exit 1 }
' "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.attach detach must CAS-publish event clears" >&2
  exit 1
fi

echo "M5 jit.attach publication guard passed"
