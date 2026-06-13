#!/bin/sh
# Build and run M5 table hash-vector retirement guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-retire

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-retire.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'TabNodeRetire' \
  'retired_nodes' \
  'tab_retire_reserve' \
  'tab_retire_arm' \
  'lj_tab_reclaim_retired' \
  'lj_tab_freeretired' \
  'lj_tab_reclaim_retired(g, epoch)' \
  'lj_gc2_reclaim_retired(g, epoch)' \
  'gc_mark_tab_retired_mem' \
  'gc2_mark_tab_retired_mem'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table retire marker: $needle" >&2
    exit 1
  fi
done

if rg -F -q 'lj_mem_freevec(g, oldnode, oldhmask+1, Node)' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table resize must retire old node vectors, not free immediately" >&2
  exit 1
fi

echo "M5 table hash-vector retirement tests passed"
