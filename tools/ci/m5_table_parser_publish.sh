#!/bin/sh
# Guard final M5 table/parser publication wrapper conversions.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LEGACY='lj_gc_(objbarrier|objbarriert|anybarriert|barrieruv|barriert|barrier)'

for file in "$ROOT/src/lj_tab.c" "$ROOT/src/lj_parse.c"; do
  hits=$(rg -n "$LEGACY\\b" "$file" || true)
  if [ -n "$hits" ]; then
    echo "guardrail: $file must use M5 publication wrappers:" >&2
    echo "$hits" >&2
    exit 1
  fi
done

for needle in \
  'copyTVrel(L, &n->key, key)' \
  'lj_gc_pubtab(L, t)' \
  'setgcrefrel(((GCRef *)kptr)[~kidx], o)' \
  'lj_gc_pubobjobj(fs->L, pt, o)' \
  'copyTVrel(fs->L, v, &tv)' \
  'lj_gc_pubtab(fs->L, t)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/src/lj_parse.c"; then
    echo "guardrail: missing table/parser publication marker: $needle" >&2
    exit 1
  fi
done

legacy_all=$(rg -n "$LEGACY\\b" "$ROOT/src"/*.c | grep -v "$ROOT/src/lj_gc.c" || true)
if [ -n "$legacy_all" ]; then
  echo "guardrail: legacy barrier call sites remain outside lj_gc.c:" >&2
  echo "$legacy_all" >&2
  exit 1
fi

echo "M5 table/parser publication guard passed"
