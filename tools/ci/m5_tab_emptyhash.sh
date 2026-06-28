#!/bin/sh
# Run the M5 empty-hash/nilnode guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TAB="$ROOT/src/lj_tab.c"
FIXTURE="$ROOT/tests/t-tab-emptyhash.c"

if hits=$(grep -R -nF 'nextnode(' "$ROOT/tests" --include='t-*.c' || true);
then
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'C table fixtures must acquire-read Node.next through lj_tab_nextnode_acq()' >&2
    exit 1
  fi
fi

if ! grep -Fq 'assert(lj_tab_nextnode_acq(nilnode) == NULL);' "$FIXTURE"; then
  printf '%s\n' 'empty-hash fixture must acquire-read nilnode.next' >&2
  exit 1
fi

if ! grep -Fq 'lj_assertL(nodebase != &G(L)->nilnode, "insert into fallback hash");' "$TAB" ||
   ! grep -Fq 'lj_assertL(freenode != &G(L)->nilnode, "store to fallback hash");' "$TAB"; then
  printf '%s\n' 'lj_tab_newkey() must assert it never inserts through the shared nilnode' >&2
  exit 1
fi

if ! awk '
  /^cTValue \* LJ_FASTCALL lj_tab_getinth\(GCtab \*t, int32_t key\)/ {
    in_fn = 1
    found = 1
    saw_snapshot = 0
    saw_empty = 0
    saw_return = 0
    saw_hash = 0
    bad = 0
  }
  in_fn && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { saw_snapshot = 1 }
  in_fn && /if \(hmask == 0\)/ {
    if (!saw_snapshot || saw_hash) bad = 1
    saw_empty = 1
  }
  in_fn && saw_empty && /return NULL/ { saw_return = 1 }
  in_fn && /hashnum_node\(node, hmask, &k\)/ {
    if (!saw_return) bad = 1
    saw_hash = 1
  }
  in_fn && /^}/ {
    if (!(saw_snapshot && saw_empty && saw_return && saw_hash) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'lj_tab_getinth() must return before probing the shared nilnode' >&2
  exit 1
fi

if ! awk '
  /^cTValue \*lj_tab_getstr\(GCtab \*t, const GCstr \*key\)/ {
    in_fn = 1
    found = 1
    saw_snapshot = 0
    saw_empty = 0
    saw_return = 0
    saw_hash = 0
    bad = 0
  }
  in_fn && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { saw_snapshot = 1 }
  in_fn && /if \(hmask == 0\)/ {
    if (!saw_snapshot || saw_hash) bad = 1
    saw_empty = 1
  }
  in_fn && saw_empty && /return NULL/ { saw_return = 1 }
  in_fn && /hashstr_node\(node, hmask, key\)/ {
    if (!saw_return) bad = 1
    saw_hash = 1
  }
  in_fn && /^}/ {
    if (!(saw_snapshot && saw_empty && saw_return && saw_hash) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'lj_tab_getstr() must return before probing the shared nilnode' >&2
  exit 1
fi

if ! awk '
  /^cTValue \*lj_tab_get\(lua_State \*L, GCtab \*t, cTValue \*key\)/ {
    in_fn = 1
    found = 1
    saw_snapshot = 0
    saw_empty = 0
    saw_return = 0
    saw_hash = 0
    bad = 0
  }
  in_fn && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { saw_snapshot = 1 }
  in_fn && /if \(hmask == 0\)/ {
    if (!saw_snapshot || saw_hash) bad = 1
    saw_empty = 1
  }
  in_fn && saw_empty && /return niltv\(L\)/ { saw_return = 1 }
  in_fn && /hashkey_node\(node, hmask, key\)/ {
    if (!saw_return) bad = 1
    saw_hash = 1
  }
  in_fn && /^}/ {
    if (!(saw_snapshot && saw_empty && saw_return && saw_hash) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'lj_tab_get() must return before probing the shared nilnode' >&2
  exit 1
fi

if ! awk '
  /^TValue \*lj_tab_newkey\(lua_State \*L, GCtab \*t, cTValue \*key\)/ {
    in_fn = 1
    found = 1
    saw_snapshot = 0
    saw_empty = 0
    saw_rehash = 0
    saw_retry_set = 0
    saw_hash = 0
    bad = 0
  }
  in_fn && /lj_tab_node_snapshot_acq\(t, &hmask\)/ { saw_snapshot = 1 }
  in_fn && /if \(hmask == 0\)/ {
    if (!saw_snapshot || saw_hash) bad = 1
    saw_empty = 1
  }
  in_fn && saw_empty && /rehashtab\(L, t, key\)/ { saw_rehash = 1 }
  in_fn && saw_rehash && /return lj_tab_set\(L, t, key\)/ {
    saw_retry_set = 1
  }
  in_fn && /hashkey_node\(nodebase, hmask, key\)/ {
    if (!saw_retry_set) bad = 1
    saw_hash = 1
  }
  in_fn && /^}/ {
    if (!(saw_snapshot && saw_empty && saw_rehash && saw_retry_set &&
	  saw_hash) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'lj_tab_newkey() must rehash empty hashes before hash probing' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_tab_emptyhash
