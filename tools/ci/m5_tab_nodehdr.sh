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
  'LJ_STATIC_ASSERT(sizeof(TabNodeHdr) == 16)' \
  'TABNODE_FREECOUNT_MASK' \
  'TABNODE_FLAG_RETIRING' \
  'lj_tab_node_hmask_acq' \
  'lj_tab_node_hdr_flags_acq' \
  'lj_tab_node_freecount_acq' \
  'lj_tab_node_free_reserve' \
  'lj_tab_node_free_release' \
  'lj_tab_node_nextgen_acq' \
  'lj_tab_node_nextgen_rel' \
  'lj_tab_node_hdr_flags_or_rel' \
  'lj_tab_node_is_retiring' \
  'lj_tab_node_snapshot_acq' \
  'lj_tab_node_hdrw' \
  'lj_tab_node_bytes' \
  'TabNodeHdr nilnodehdr' \
  'offsetof(global_State, nilnode)' \
  'tab_node_new' \
  'hdr->flags = (hmask + 1u) & TABNODE_FREECOUNT_MASK' \
  'setmref(hdr->next_gen, NULL)' \
  'g->nilnodehdr.flags = 0' \
  'setmref(g->nilnodehdr.next_gen, NULL)' \
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

if ! rg -F -q 'lj_tab_node_freecount_acq(newnode) == newhmask + 1u - 5u' \
    "$ROOT/tests/t-tab-nodehdr.c"; then
  echo "guardrail: node-header test must assert rebuilt node freecount" >&2
  exit 1
fi

if ! rg -F -q 'lj_tab_node_nextgen_acq(oldnode) == newnode' \
    "$ROOT/tests/t-tab-nodehdr.c"; then
  echo "guardrail: node-header test must assert retired node next_gen" >&2
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

if rg -F -n 'tab_node_retry_if_retiring' "$ROOT/src"; then
  echo "guardrail: table node retiring retry must be centralized in lj_tab_node_snapshot_acq" >&2
  exit 1
fi

if ! awk '
  /TValue \*lj_tab_newkey\(lua_State \*L,/ { innewkey = 1; next }
  innewkey && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { newkey = 1 }
  innewkey && /^}/ { innewkey = 0 }
  /int lj_tab_try_newkey_anchor\(lua_State \*L,/ { inanchor = 1; next }
  inanchor && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { anchor = 1 }
  inanchor && /^}/ { inanchor = 0 }
  /int lj_tab_try_newkey_chain\(lua_State \*L,/ { inchain = 1; next }
  inchain && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { chain = 1 }
  inchain && /^}/ { inchain = 0 }
  /TValue \*lj_tab_setinth\(lua_State \*L,/ { inseti = 1; next }
  inseti && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { seti = 1 }
  inseti && /^}/ { inseti = 0 }
  /TValue \*lj_tab_setstr\(lua_State \*L,/ { insets = 1; next }
  insets && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { sets = 1 }
  insets && /^}/ { insets = 0 }
  /TValue \*lj_tab_set\(lua_State \*L,/ { inset = 1; next }
  inset && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { set = 1 }
  inset && /^}/ { inset = 0 }
  END { exit newkey && anchor && chain && seti && sets && set ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table write probes must enter through node snapshots" >&2
  exit 1
fi

if ! awk '
  /static LJ_AINLINE void clearhpart\(GCtab \*t\)/ ||
  /void LJ_FASTCALL lj_tab_clear\(GCtab \*t\)/ ||
  /void LJ_FASTCALL lj_tab_free\(global_State \*g, GCtab \*t\)/ {
    infn = 1
    snap = bad = 0
    next
  }
  infn && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { snap = 1 }
  infn && /lj_tab_node_acq\(t\)|lj_tab_node_hmask_acq\(node\)/ { bad = 1 }
  infn && /^}/ {
    if (!snap || bad)
      fail = 1
    infn = 0
  }
  END { exit fail ? 1 : 0 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table clear/free hash readers must use node snapshots" >&2
  exit 1
fi

if ! awk '
  /void lj_tab_resize\(lua_State \*L,/ { inresize = 1 }
  inresize && /lj_tab_node_nextgen_rel\(oldnode,/ { nextgen = NR }
  inresize && /lj_tab_node_hdr_flags_or_rel\(oldnode, TABNODE_FLAG_RETIRING\)/ { retiring = NR }
  inresize && /^}/ { inresize = 0 }
  END { exit nextgen && retiring && nextgen < retiring ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: resize must publish retired node next_gen before RETIRING" >&2
  exit 1
fi

if ! awk '
  /void lj_tab_resize\(lua_State \*L,/ {
    inresize = 1
    snap = bad = 0
    next
  }
  inresize && /oldnode = lj_tab_node_snapshot_acq\(t, &oldhmask\)/ {
    snap = 1
  }
  inresize && /lj_tab_node_acq\(t\)|lj_tab_node_hmask_acq\(oldnode\)/ {
    bad = 1
  }
  inresize && /^}/ {
    checked = 1
    inresize = 0
  }
  END { exit checked && snap && !bad ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: resize must snapshot node generation headers" >&2
  exit 1
fi

echo "M5 table hash-vector header tests passed"
