#!/bin/sh
# Build and guard M5 string table CAS publication scaffolding.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-strtab-cas

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-strtab-cas.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'LJ_STRTAB_RESIZE' \
  'strtab_enter' \
  'strtab_leave' \
  'strtab_retire' \
  'retired_next' \
  'g->str.retired' \
  'gc_mark_strtab_mem' \
  'gc2_mark_strtab_mem' \
  'LJ_STRTAB_ACTIVE_MASK' \
  'state | LJ_STRTAB_RESIZE' \
  'while (la_load32_acq(&oldhdr->resize) & LJ_STRTAB_ACTIVE_MASK)' \
  'strref_cas_rel' \
  'la_storeptr_rel((void **)&g->str.tabh' \
  'la_add32_rlx(&g->str.num'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing string table CAS marker: $needle" >&2
    exit 1
  fi
done

if rg -F -q 'lj_mem_free(g, oldhdr' "$ROOT/src/lj_str.c"; then
  echo "guardrail: string table resize must retire old headers, not free them immediately" >&2
  exit 1
fi

echo "M5 string table CAS publication tests passed"
