#!/bin/sh
# Run the M5 table node-header guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OBJ="$ROOT/src/lj_obj.h"
STATE="$ROOT/src/lj_state.c"
TAB="$ROOT/src/lj_tab.c"

for marker in \
  'typedef struct TabNodeHdr {' \
  'MSize hmask;' \
  'MSize flags;' \
  'MRef next_gen;' \
  '#define TABNODE_FREECOUNT_MASK' \
  '#define TABNODE_FLAGS_MASK' \
  '#define TABNODE_FLAG_RETIRING' \
  'LJ_STATIC_ASSERT(sizeof(TabNodeHdr) == 16);' \
  'TabNodeHdr nilnodehdr;' \
  'offsetof(global_State, nilnodehdr) + sizeof(TabNodeHdr)'
do
  if ! grep -Fq "$marker" "$OBJ"; then
    printf 'required table node-header marker missing: %s\n' "$marker" >&2
    exit 1
  fi
done

for helper in \
  'lj_tab_node_hdr(' \
  'lj_tab_node_hdrw(' \
  'lj_tab_node_bytes(' \
  'lj_tab_node_hmask_acq(' \
  'lj_tab_node_hdr_flags_acq(' \
  'lj_tab_node_hdr_flags_word_acq(' \
  'lj_tab_node_hdr_flags_word_cas(' \
  'lj_tab_node_freecount_acq(' \
  'lj_tab_node_freecount_set_rel(' \
  'lj_tab_node_free_reserve(' \
  'lj_tab_node_nextgen_acq(' \
  'lj_tab_node_nextgen_cas(' \
  'lj_tab_node_is_retiring('
do
  if ! grep -Fq "$helper" "$OBJ"; then
    printf 'required table node-header helper missing: %s\n' "$helper" >&2
    exit 1
  fi
done

if ! awk '
  /^static LJ_AINLINE Node \*tab_node_new\(/ {
    in_fn = 1
    found = 1
    saw_hdr = 0
    saw_bytes = 0
    saw_after_hdr = 0
    saw_hmask = 0
    saw_flags = 0
    saw_next = 0
  }
  in_fn && /TabNodeHdr \*hdr =/ { saw_hdr = 1 }
  in_fn && /lj_mem_new\(L, lj_tab_node_bytes\(hmask\)\)/ { saw_bytes = 1 }
  in_fn && /sizeof\(TabNodeHdr\)/ { saw_after_hdr = 1 }
  in_fn && /hdr->hmask = hmask/ { saw_hmask = 1 }
  in_fn && /hdr->flags = \(hmask \+ 1u\) & TABNODE_FREECOUNT_MASK/ {
    saw_flags = 1
  }
  in_fn && /setmref\(hdr->next_gen, NULL\)/ { saw_next = 1 }
  in_fn && /^}/ {
    if (!(saw_hdr && saw_bytes && saw_after_hdr && saw_hmask &&
	  saw_flags && saw_next))
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'tab_node_new() must allocate and initialize a TabNodeHdr before Node[0]' >&2
  exit 1
fi

for marker in \
  'g->nilnodehdr.hmask = 0;' \
  'g->nilnodehdr.flags = 0;' \
  'setmref(g->nilnodehdr.next_gen, NULL);'
do
  if ! grep -Fq "$marker" "$STATE"; then
    printf 'nilnode header initialization missing: %s\n' "$marker" >&2
    exit 1
  fi
done

if ! awk '
  /^void lj_tab_resize\(lua_State \*L, GCtab \*t, uint32_t asize, uint32_t hbits\)/ {
    in_fn = 1
    found = 1
    saw_flags0 = 0
    saw_retiring_wait = 0
    saw_next_cas = 0
    saw_flag_cas = 0
    saw_revert = 0
    saw_push = 0
    bad = 0
  }
  in_fn && /hash_flags0 = oldhmask > 0 \? lj_tab_node_hdr_flags_word_acq\(oldnode\) : 0/ {
    saw_flags0 = 1
  }
  in_fn && /hash_flags0 & \(uint32_t\)TABNODE_FLAG_RETIRING/ {
    saw_retiring_wait = 1
  }
  in_fn && /lj_tab_node_nextgen_cas\(oldnode, &expect_next, node_succ\)/ {
    saw_next_cas = 1
  }
  in_fn && /lj_tab_node_hdr_flags_word_cas\(oldnode, &expect_flags, want_flags\)/ {
    if (!saw_next_cas) bad = 1
    saw_flag_cas = 1
  }
  in_fn && /lj_tab_node_nextgen_cas\(oldnode, &revert, NULL\)/ {
    if (!saw_flag_cas) bad = 1
    saw_revert = 1
  }
  in_fn && /tab_retired_push\(g, oldret\)/ {
    if (!saw_flag_cas) bad = 1
    saw_push = 1
  }
  in_fn && /^}/ {
    if (!(saw_flags0 && saw_retiring_wait && saw_next_cas && saw_flag_cas &&
	  saw_revert && saw_push) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'lj_tab_resize() must claim next_gen before publishing RETIRING and must revert failed claims' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_tab_nodehdr
