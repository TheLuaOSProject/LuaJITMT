#!/bin/sh
# Guard Lua and ctype metamethod lookup snapshots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for needle in \
  'cTValue *lj_meta_cachetv(GCtab *mt, MMS mm, GCstr *name,' \
  'lj_tv_load_acq(out, mo)' \
  'cTValue *lj_meta_lookuptv(lua_State *L, TValue *out,' \
  'lj_meta_fasttv(g, mt, mm, out)' \
  'cTValue *lj_ctype_metatv(CTState *cts, TValue *out,' \
  'lj_tv_load_acq(&tabv, tv)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_meta.c" "$ROOT/src/lj_meta.h" \
      "$ROOT/src/lj_ctype.c" "$ROOT/src/lj_ctype.h"; then
    echo "guardrail: missing metamethod snapshot marker: $needle" >&2
    exit 1
  fi
done

lookup_hits=$(rg -n 'lj_meta_lookup\(' "$ROOT/src" -g '*.c' -g '*.h' |
  grep -v "$ROOT/src/lj_meta.c:.*cTValue \\*lj_meta_lookup" |
  grep -v "$ROOT/src/lj_meta.h:.*lj_meta_lookup" || true)
if [ -n "$lookup_hits" ]; then
  echo "guardrail: Lua metamethod users must use lj_meta_lookuptv:" >&2
  echo "$lookup_hits" >&2
  exit 1
fi

fast_hits=$(rg -n 'lj_meta_fast(g)?\(' "$ROOT/src" -g '*.c' -g '*.h' |
  grep -v "$ROOT/src/lj_meta.h:" |
  grep -v "$ROOT/src/lj_meta.c:.*See the lj_meta_fast" || true)
if [ -n "$fast_hits" ]; then
  echo "guardrail: Lua fast metamethod users must use lj_meta_fasttv:" >&2
  echo "$fast_hits" >&2
  exit 1
fi

ctype_hits=$(rg -n 'lj_ctype_meta\(' "$ROOT/src" -g '*.c' -g '*.h' |
  grep -v "$ROOT/src/lj_ctype.c:.*cTValue \\*lj_ctype_meta" |
  grep -v "$ROOT/src/lj_ctype.h:.*lj_ctype_meta" || true)
if [ -n "$ctype_hits" ]; then
  echo "guardrail: ctype metamethod users must use lj_ctype_metatv:" >&2
  echo "$ctype_hits" >&2
  exit 1
fi

for file in "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; do
  if ! rg -F -q 'mode = lj_meta_fasttv(g, mt, MM_mode, &modev)' "$file"; then
    echo "guardrail: weak-table __mode lookup must snapshot: $file" >&2
    exit 1
  fi
done

if ! rg -F -q 'mo = lj_meta_fasttv(g, tabref(gco2ud(o)->metatable), MM_gc, &motv)' \
    "$ROOT/src/lj_gc.c"; then
  echo "guardrail: userdata __gc finalizer lookup must snapshot" >&2
  exit 1
fi

echo "M5 metamethod snapshot guard passed"
