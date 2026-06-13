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
  'tabslot_load_acq' \
  'tv_rawload_acq(src)' \
  'tabslot_isnil_acq' \
  'tabslot_load_acq(&nk, &n->key)' \
  'tabslot_load_acq(&val, &n->val)' \
  'tabslot_load_acq(&key, &n->key)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c"; then
    echo "guardrail: missing table slot snapshot marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_obj_equal\(&n->key|tvisnum\(&n->key|tvisstr\(&n->key|strV\(&n->key|n->key\.n|tvisnil\(&n->val' \
    "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table hash key/value decisions must use snapshots" >&2
  exit 1
fi

echo "M5 table hash-node TValue snapshot tests passed"
