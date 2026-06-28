#!/bin/sh
# Run the M5 table KEYLOCK lookup and resize guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SRC="$ROOT/src/lj_tab.c"

if ! awk '
  /^static uint32_t tab_rehash_hashcount\(/ {
    in_fn = 1
    saw_key_load = 0
    saw_keylock = 0
    saw_wait = 0
    saw_count = 0
    bad = 0
  }
  in_fn && /lj_tv_load_acq\(&key, &n->key\)/ { saw_key_load = 1 }
  in_fn && /tab_key_islocked\(&key\)/ { saw_keylock = 1 }
  in_fn && /lj_tab_wait_no_l[[:space:]]*[(]/ { saw_wait = 1 }
  in_fn && /count\+\+/ {
    saw_count = 1
    if (!saw_key_load || !saw_keylock || !saw_wait)
      bad = 1
  }
  in_fn && /^}/ {
    if (!saw_key_load || !saw_keylock || !saw_wait || !saw_count || bad)
      exit 1
    in_fn = 0
  }
' "$SRC"; then
  printf '%s\n' 'tab_rehash_hashcount() must wait out KEYLOCK keys before counting resize hash entries' >&2
  exit 1
fi

if ! awk '
  /^void lj_tab_resize\(lua_State \*L, GCtab \*t, uint32_t asize, uint32_t hbits\)/ {
    in_resize = 1
    in_hash = 0
    saw_key_load = 0
    saw_keylock = 0
    saw_wait = 0
    saw_freeze = 0
    saw_hidden_filter = 0
    saw_migrate = 0
    bad = 0
  }
  in_resize && /if[[:space:]]*[(]oldhmask[[:space:]]*>[[:space:]]*0[)][[:space:]]*[{].*Reinsert pairs/ {
    in_hash = 1
  }
  in_hash && /lj_tv_load_acq\(&key, &n->key\)/ { saw_key_load = 1 }
  in_hash && /tab_key_islocked\(&key\)/ { saw_keylock = 1 }
  in_hash && /lj_tab_wait_no_l[[:space:]]*[(]/ { saw_wait = 1 }
  in_hash && /tab_freeze_forward\(&n->val, &val\)/ {
    saw_freeze = 1
    if (!saw_key_load || !saw_keylock || !saw_wait)
      bad = 1
  }
  in_hash && /tab_hash_key_hidden\(&key\)/ { saw_hidden_filter = 1 }
  in_hash && /tab_migrate_store_if_absent[[:space:]]*[(]/ {
    saw_migrate = 1
    if (!saw_freeze || !saw_hidden_filter)
      bad = 1
  }
  in_hash && /^  if[[:space:]]*[(]!oldarray_separated/ {
    in_hash = 0
  }
  in_resize && /^}/ {
    if (!saw_key_load || !saw_keylock || !saw_wait || !saw_freeze ||
	!saw_hidden_filter || !saw_migrate || bad)
      exit 1
    in_resize = 0
  }
' "$SRC"; then
  printf '%s\n' 'lj_tab_resize() must wait out KEYLOCK keys before freezing and migrating old hash values' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_tab_keylock_lookup
