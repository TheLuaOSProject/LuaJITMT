#!/bin/sh
# Build and run M5 table hash-vector publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-node-publish

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-node-publish.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'lj_tab_node_acq' \
  'lj_tab_node_rel' \
  'la_load64_acq(&t->node.ptr64)' \
  'la_store64_rel(&t->node.ptr64' \
  'hashmask(const GCtab *t, uint32_t hash)' \
  'Node *n = lj_tab_node_acq(t)' \
  'lj_tab_node_rel(t, node)' \
  'lj_tab_node_rel(t, &g->nilnode)' \
  'newhpart_alloc' \
  'newhpart_publish' \
  'tab_rehash_hashcount' \
  'tab_rehash_arrayslot' \
  'tab_rehash_slot' \
  'tab_rehash_insert'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table node publication marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'noderef\(([^)]*->)?node\)|setmref\(([^,]*->)?node' \
    "$ROOT/src" --glob '!lj_obj.h' --glob '!host/*'; then
  echo "guardrail: table node vectors must use lj_tab_node_* helpers" >&2
  exit 1
fi

if awk '
  /void lj_tab_resize/ { in_resize = 1 }
  in_resize && /^}/ { in_resize = 0 }
  in_resize && /newhpart\(L, t, hbits\)/ { bad_legacy = 1 }
  in_resize && /lj_tab_set(inth)?\(L, t,/ { bad_legacy = 1 }
  in_resize && /tab_rehash_hashcount\(oldnode, oldhmask, oldarray,/ {
    count = NR
  }
  in_resize && /tab_rehash_slot\(L, array, asize, newnode, newhmask,/ {
    route = NR
  }
  in_resize && /newhpart_publish\(t, newnode, newhmask, newfreetop\)/ {
    publish = NR
  }
  in_resize && /tab_retire_arm\(G\(L\), oldret\)/ { arm = NR }
  END {
    if (bad_legacy || !count || !route || !publish || route > publish || !arm || arm < publish)
      exit 1
    exit 0
  }
' "$ROOT/src/lj_tab.c"; then
  :
else
  echo "guardrail: resize must rebuild hash vectors before publication" >&2
  exit 1
fi

echo "M5 table hash-vector publication tests passed"
