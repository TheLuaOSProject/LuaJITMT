#!/bin/sh
# Build and run M5 table array publication/retirement guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-array-publish

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-array-publish.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'lj_tab_array_acq' \
  'lj_tab_array_rel' \
  'lj_tab_asize_acq' \
  'lj_tab_asize_rel' \
  'TabArrayHdr' \
  'lj_tab_array_hdrw' \
  'lj_tab_array_bytes' \
  'lj_tab_array_mem_acq' \
  'TabArrayRetire' \
  'retired_arrays' \
  'tab_array_new' \
  'tab_array_free' \
  'tab_array_retire_reserve' \
  'tab_array_retire_arm' \
  'lj_tab_array_rel(t, array)' \
  'lj_tab_asize_rel(t, asize)' \
  'lj_tab_array_hdr_asize_rel(array, asize)' \
  'lj_tv_load_acq(&val, &array[i])'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table array publication marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'tvref\([^[:space:]]*->array\)|setmref\([^,]*->array' \
    "$ROOT/src" --glob '!lj_obj.h' --glob '!host/*' --glob '!vm_*.dasc' \
    --glob '!lj_asm_*.h'; then
  echo "guardrail: table arrays must use lj_tab_array_* helpers in C code" >&2
  exit 1
fi

if rg -n 'lj_mem_realloc(vec)?\([^;]*(array|t->array|oldarray)' \
    "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table resize must retire old arrays, not realloc them" >&2
  exit 1
fi

echo "M5 table array publication tests passed"
