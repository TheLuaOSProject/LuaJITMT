#!/bin/sh
# Build and run M5 table hash-node TValue snapshot guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-slot-snapshot

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-slot-snapshot.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'lj_tv_load_acq' \
  'tv_rawload_acq(src)' \
  'lj_tv_isnil_acq' \
  'lj_tv_load_acq(&nk, &n->key)' \
  'lj_tv_load_acq(&val, &n->val)' \
  'lj_tv_load_acq(&key, &n->key)' \
  'BCWriteHashSnap' \
  'gc_marktv(g, &key)' \
  'gc2_mark_tv_worker(g, &key)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table slot snapshot marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_obj_equal\(&n->key|tvisnum\(&n->key|tvisstr\(&n->key|strV\(&n->key|numV\(&n->key|gcV\(&n->key|itype2irt\(&n->key|n->key\.n|tvisnil\(&n->val|tvisnil\(&node->val|tvisnil\(&hashnode\[[^]]+\]\.val|tvisnil\(&node\[[^]]+\]\.val|numberVnum\(&node\[[^]]+\]\.key|serialize_put\(w, sbx, &node->key|bcwrite_ktabk\(ctx, &node->key|gc_marktv\(g, &n->key|gc2_mark_tv_worker\(g, &n->key|gc_mayclear\(g, &n->key|copyTV\(L, &tmp, &node\[[^]]+\]\.val' \
    "$ROOT/src/lj_tab.c" \
    "$ROOT/src/lib_table.c" \
    "$ROOT/src/lj_bcwrite.c" \
    "$ROOT/src/lj_serialize.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_record.c" \
    "$ROOT/src/lj_parse.c"; then
  echo "guardrail: table hash key/value decisions must use snapshots" >&2
  exit 1
fi

echo "M5 table hash-node TValue snapshot tests passed"
