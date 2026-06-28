#!/bin/sh
# Run the M5 stable table node/hash-chain publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OBJ="$ROOT/src/lj_obj.h"
TAB="$ROOT/src/lj_tab.c"

for marker in \
  'static LJ_AINLINE Node *lj_tab_nextnode_acq(const Node *n)' \
  'la_load64_acq(&n->next.ptr64)' \
  'la_load32_acq(&n->next.ptr32)' \
  'static LJ_AINLINE void lj_tab_nextnode_rel(Node *n, const Node *next)' \
  'la_store64_rel(&n->next.ptr64' \
  'la_store32_rel(&n->next.ptr32'
do
  if ! grep -Fq "$marker" "$OBJ"; then
    printf 'required table next-node ordering helper missing: %s\n' "$marker" >&2
    exit 1
  fi
done

if ! grep -Fq '#define LJ_TAB_MAXCHAIN		8u' "$TAB"; then
  printf '%s\n' 'LJ_TAB_MAXCHAIN must remain pinned for resize-on-long-chain coverage' >&2
  exit 1
fi

if hits=$(grep -nE 'Brent|relocat|othern|mainpos|main position|[*][[:space:]]*freenode[[:space:]]*=[[:space:]]*[*][[:space:]]*n|[*][[:space:]]*n[[:space:]]*=[[:space:]]*[*][[:space:]]*freenode' \
    "$TAB" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'table insertion must not reintroduce legacy move/Brent relocation paths' >&2
  exit 1
fi

if ! awk '
  /^static TValue \*tab_findkey_or_keylock\(/ {
    in_fn = 1
    found = 1
    saw_acq_walk = 0
    saw_key_load = 0
    saw_keylock = 0
  }
  in_fn && /for \(n = anchor; n != NULL; n = lj_tab_nextnode_acq\(n\)\)/ {
    saw_acq_walk = 1
  }
  in_fn && /lj_tv_load_acq\(&nk, &n->key\)/ { saw_key_load = 1 }
  in_fn && /tab_key_islocked\(&nk\)/ { saw_keylock = 1 }
  in_fn && /^}/ {
    if (!(saw_acq_walk && saw_key_load && saw_keylock))
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'tab_findkey_or_keylock() must acquire-walk links and snapshot keys' >&2
  exit 1
fi

if ! awk '
  /^TValue \*lj_tab_newkey\(lua_State \*L, GCtab \*t, cTValue \*key\)/ {
    in_fn = 1
    found = 1
    find_calls = 0
    saw_maxchain = 0
    saw_chain_overflow = 0
    saw_claim_free = 0
    saw_set_next = 0
    saw_store_key = 0
    saw_collision_barrier = 0
    saw_collision_pubtab = 0
    saw_rel_next = 0
    bad = 0
  }
  in_fn && /tab_findkey_or_keylock\(n, key, &locked, &chainlen\)/ {
    find_calls++
  }
  in_fn && /chainlen >= LJ_TAB_MAXCHAIN/ { saw_maxchain = 1 }
  in_fn && /tab_rehash_chain_overflow\(L, t, key, hmask\)/ {
    if (!saw_maxchain) bad = 1
    saw_chain_overflow = 1
  }
  in_fn && /tab_claim_free_node_scan\(nodebase, hmask, n, &locked\)/ {
    saw_claim_free = 1
  }
  in_fn && /lj_tab_nextnode_set\(freenode, lj_tab_nextnode_acq\(n\)\)/ {
    if (!saw_claim_free) bad = 1
    saw_set_next = 1
  }
  in_fn && /tab_storekeyrel\(L, &freenode->key, key\)/ {
    if (!saw_set_next) bad = 1
    saw_store_key = 1
  }
  in_fn && /lj_gc2_barrier_weak_key\(L, t, key\)/ {
    if (saw_set_next) {
      if (!saw_store_key) bad = 1
      saw_collision_barrier = 1
    }
  }
  in_fn && /lj_gc_pubtab\(L, t\)/ {
    if (saw_set_next) {
      if (!saw_collision_barrier) bad = 1
      saw_collision_pubtab = 1
    }
  }
  in_fn && /lj_tab_nextnode_rel\(n, freenode\)/ {
    if (!saw_collision_pubtab) bad = 1
    saw_rel_next = 1
  }
  in_fn && /^}/ {
    if (!(find_calls >= 2 && saw_maxchain && saw_chain_overflow &&
	  saw_claim_free && saw_set_next && saw_store_key &&
	  saw_collision_barrier && saw_collision_pubtab && saw_rel_next) ||
	bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'lj_tab_newkey() must initialize and barrier free nodes before release-linking them' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_tab_chain_order
