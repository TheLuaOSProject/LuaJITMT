#!/bin/sh
# Build and guard M5 string table CAS publication scaffolding.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-strtab-cas
OUT_REHASH=${TMPDIR:-/tmp}/lj_t-strtab-rehash

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-strtab-cas.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-strtab-rehash.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT_REHASH"
timeout 20s "$OUT_REHASH"

for needle in \
  'LJ_STRTAB_RESIZE' \
  'strtab_enter' \
  'strtab_leave' \
  'strtab_claim' \
  'strtab_release' \
  'strtab_retire' \
  'retired_next' \
  'retire_epoch' \
  'g->str.retired' \
  'lj_str_reclaim_retired' \
  'la_xchgptr_acqrel((void **)&g->str.retired' \
  'la_load64_acq(&hdr->retire_epoch) < completed_epoch' \
  'lj_str_reclaim_retired(g, epoch)' \
  'gc_mark_strtab_mem' \
  'gc2_mark_strtab_mem' \
  'LJ_STRTAB_ACTIVE_MASK' \
  'state | LJ_STRTAB_RESIZE' \
  'while (la_load32_acq(&hdr->resize) & LJ_STRTAB_ACTIVE_MASK)' \
  'strtab_claim(hdr)' \
  'strtab_release(hdr)' \
  'strref_cas_rel' \
  'la_storeptr_rel((void **)&g->str.tabh' \
  'la_add32_rlx(&g->str.num' \
  'la_sub32_acqrel(&g->str.num' \
  'la_load32_acq(&g->str.num)' \
  'la_add32_rlx(&g->str.id'
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

if [ "$(rg -F 'lj_mem_free(g, hdr, lj_str_tabbytes(hdr))' "$ROOT/src/lj_str.c" | wc -l)" -ne 3 ]; then
  echo "guardrail: retired string table headers should only be freed by reclaim and state close" >&2
  exit 1
fi

if rg -n 'g->str\.num--|\+\+g->str\.num|if \(g->str\.num|g->str\.num <=|g->str\.num \+=' \
    "$ROOT/src/lj_str.c" "$ROOT/src/lj_gc.c"; then
  echo "guardrail: string count must use atomic add/sub/load helpers" >&2
  exit 1
fi

if rg -n 'g->str\.id\+\+|\+\+g->str\.id|g->str\.id =|g->str\.idreseed|idreseed--' \
    "$ROOT/src/lj_str.c"; then
  echo "guardrail: string IDs must not mutate global id/reseed state in allocation" >&2
  exit 1
fi

echo "M5 string table CAS publication tests passed"
