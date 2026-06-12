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
  /LJLIB_CF\(jit_profile_start\)/ { infn = 1; next }
  infn && /copyTVrel\(L, lj_tab_set\(L, registry, &key\), &tv\)/ { rel++ }
  infn && /lj_gc_pubtab\(L, registry\)/ { pub = 1 }
  infn && /^}/ { exit(rel >= 2 && pub ? 0 : 1) }
  END { if (rel < 2 || !pub) exit 1 }
' "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.profile start must release-publish registry anchors" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(jit_profile_stop\)/ { infn = 1; next }
  infn && /lj_gc_pubtab\(L, registry\)/ { pub = 1 }
  infn && /^}/ { exit(pub ? 0 : 1) }
  END { if (!pub) exit 1 }
' "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.profile stop must publish registry clears" >&2
  exit 1
fi

echo "M5 jit.profile publication guard passed"
