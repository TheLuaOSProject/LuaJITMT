#!/bin/sh
# Run the Lua-defined M5 table FORWARD value filtering guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

awk '
  /^void lj_tab_resize\(lua_State \*L, GCtab \*t, uint32_t asize, uint32_t hbits\)/ {
    in_resize = 1
  }
  in_resize && /tab_freeze_forward\(&oldarray\[i\], &val\)/ {
    saw_array_freeze = 1
  }
  in_resize && /tab_freeze_forward\(&n->val, &val\)/ {
    saw_hash_freeze = 1
  }
  in_resize && /copyTVrel\(L, slot, &val\)/ {
    bad_copy = 1
  }
  in_resize && /^}/ {
    in_resize = 0
  }
  END {
    if (!saw_array_freeze || !saw_hash_freeze || bad_copy)
      exit 1
  }
' "$ROOT/src/lj_tab.c" || {
  printf '%s\n' 'lj_tab_resize must CAS-forward old array/hash slots before migration' >&2
  exit 1
}

exec "$ROOT/tools/ci/lua_test.sh" m5_tab_forward_filter
