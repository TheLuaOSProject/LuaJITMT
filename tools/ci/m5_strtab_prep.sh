#!/bin/sh
# Build and guard M5 string table representation prep.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-strtab-prep

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-strtab-prep.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

if rg -n 'g->str\.tab([^h[:alnum:]_]|$)|str\.tab([^h[:alnum:]_]|$)' \
  "$ROOT/src" "$ROOT/tests"
then
  echo "guardrail: string table users must route through g->str.tabh" >&2
  exit 1
fi

for needle in \
  'typedef struct StrTabHdr' \
  'StrTabHdr *tabh' \
  '#define LJ_STRHASH_DEAD' \
  '#define LJ_STRHASH_SECONDARY' \
  '#define LJ_STRHASH_LINKMASK'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h" "$ROOT/src/lj_str.h"; then
    echo "guardrail: missing string table representation marker: $needle" >&2
    exit 1
  fi
done

if rg -n '& ~(uintptr_t)1|& 1\)|\| \(u & 1\)|\(uintptr_t\)1\)' \
  "$ROOT/src/lj_str.c" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"
then
  echo "guardrail: string hash marker code must not use raw bit0 masks" >&2
  exit 1
fi

echo "M5 string table representation prep tests passed"
