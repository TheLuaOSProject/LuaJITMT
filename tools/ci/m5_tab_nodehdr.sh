#!/bin/sh
# Build and run M5 table hash-vector header guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-nodehdr

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-nodehdr.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'typedef struct TabNodeHdr' \
  'TABNODE_FLAG_RETIRING' \
  'lj_tab_node_hmask_acq' \
  'lj_tab_node_hdr_flags_acq' \
  'lj_tab_node_hdr_flags_or_rel' \
  'tab_node_is_retiring' \
  'tab_node_retry_if_retiring' \
  'lj_tab_node_hdrw' \
  'lj_tab_node_bytes' \
  'TabNodeHdr nilnodehdr' \
  'offsetof(global_State, nilnode)' \
  'tab_node_new' \
  'hdr->flags = 0' \
  'g->nilnodehdr.flags = 0' \
  'tab_node_free'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table node-header marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING' \
    "$ROOT/tests/t-tab-nodehdr.c"; then
  echo "guardrail: node-header test must assert retired node flags" >&2
  exit 1
fi

if rg -n 'nilnodehdr\.unused|hdr->unused|\.unused = 0' "$ROOT/src"; then
  echo "guardrail: table node header state must use flags, not unused" >&2
  exit 1
fi

if rg -n 'lj_mem_freevec\(g, [^,]*node|lj_mem_newvec\(L, [^,]*, Node\)' \
    "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table node vectors must allocate/free with TabNodeHdr base" >&2
  exit 1
fi

echo "M5 table hash-vector header tests passed"
