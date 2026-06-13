#!/bin/sh
# Guard allocator accounting bridge required before removing JIT GCSTEP.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-gc2-alloc-account

make -C "$ROOT/src" >/dev/null

for needle in \
  'uint64_t alloc_since_trigger' \
  'uint64_t local_total' \
  'LJ_GC2_ACCT_FLUSH' \
  'la_xchg64_acqrel' \
  'lj_gc2_account_alloc(global_State *g, TGState *tg, GCSize bytes)' \
  'lj_gc2_flush_alloc(global_State *g, TGState *tg)' \
  'la_add64_rlx(&tg->local_total' \
  'la_add64_rlx(&g->gc2.alloc_since_trigger' \
  'lj_gc2_account_alloc(g, L2TG(L), nsz - osz)' \
  'lj_gc2_account_alloc(g, L2TG(L), size)' \
  'lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 safepoint flush. */' \
  'lj_gc2_flush_alloc(g, tg);  /* 04 section 4.8 detach accounting. */'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_atomic.h" "$ROOT/src/lj_obj.h" \
      "$ROOT/src/lj_gc2.h" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_gc.c" \
      "$ROOT/src/lj_safepoint.c" "$ROOT/src/lj_tg.c" "$ROOT/src/lj_tg.h"; then
    echo "guardrail: missing allocator accounting marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'm6_jit_alloc_account.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_alloc_account.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-alloc-account.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

echo "M6 JIT allocator accounting guard passed"
