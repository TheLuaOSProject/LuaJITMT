#!/bin/sh
# Build and run M5 stable table-node/hash-chain ordering guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-chain-order

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-chain-order.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'lj_tab_nextnode_acq' \
  'lj_tab_nextnode_rel' \
  'la_load64_acq(&n->next.ptr64)' \
  'la_store64_rel(&n->next.ptr64' \
  'Nodes are never moved within a hash generation' \
  'lj_tab_nextnode_rel(n, freenode)' \
  'tab_storekeyrel(L, &freenode->key, key)' \
  'return &freenode->val' \
  'lj_tab_nextnode_acq(n)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table chain ordering marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'nextnode\(|setmref\([^,]+->next|setmrefr\([^,]+->next|noderef\([^[:cntrl:]]*->next' \
    "$ROOT/src/lj_tab.c" "$ROOT/src/lj_serialize.c" | rg -v 'next_gen'; then
  echo "guardrail: table hash-chain walks/stores must use ordered helpers" >&2
  exit 1
fi

if rg -n 'freenode->val = n->val|freenode->key = n->key|Colliding node not the main node|Use Brent' \
    "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table insertion must not move existing hash nodes" >&2
  exit 1
fi

echo "M5 stable table-node/hash-chain ordering tests passed"
