#!/bin/sh
# Guard final M5 table/parser publication wrapper conversions.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LEGACY='lj_gc_(objbarrier|objbarriert|anybarriert|barrieruv|barriert|barrier)'

for file in "$ROOT/src/lj_tab.c" "$ROOT/src/lj_parse.c" "$ROOT/src/lj_bcread.c"; do
  hits=$(rg -n "$LEGACY\\b" "$file" || true)
  if [ -n "$hits" ]; then
    echo "guardrail: $file must use M5 publication wrappers:" >&2
    echo "$hits" >&2
    exit 1
  fi
done

for needle in \
  'tab_storekeyrel(L, &n->key, key)' \
  'copyTVrel(L, dst, &k)' \
  'lj_gc_pubtab(L, t)' \
  'setgcrefrel(pt->chunkname, obj2gco(ls->chunkname));' \
  'lj_gc_pubobjobj(L, pt, ls->chunkname);' \
  'setgcrefrel(((GCRef *)kptr)[~kidx], o)' \
  'lj_gc_pubobjobj(fs->L, pt, o)' \
  'setgcrefrel(*kr, o);' \
  'lj_gc_pubobjobj(ls->L, pt, o);' \
  'lj_gc_pubobjobj(ls->L, pt, ls->chunkname);' \
  'copyTVrel(fs->L, v, &tv)' \
  'lj_gc_pubtab(fs->L, t)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/src/lj_parse.c" \
      "$ROOT/src/lj_bcread.c"; then
    echo "guardrail: missing table/parser publication marker: $needle" >&2
    exit 1
  fi
done

raw_proto_ref_hits=$(rg -n 'setgcref\((pt->chunkname|\*kr)' \
  "$ROOT/src/lj_parse.c" "$ROOT/src/lj_bcread.c" || true)
if [ -n "$raw_proto_ref_hits" ]; then
  echo "guardrail: proto chunkname/KGC refs must use release stores:" >&2
  echo "$raw_proto_ref_hits" >&2
  exit 1
fi

legacy_all=$(rg -n "$LEGACY\\b" "$ROOT/src"/*.c | grep -v "$ROOT/src/lj_gc.c" || true)
if [ -n "$legacy_all" ]; then
  echo "guardrail: legacy barrier call sites remain outside lj_gc.c:" >&2
  echo "$legacy_all" >&2
  exit 1
fi

echo "M5 table/parser publication guard passed"
