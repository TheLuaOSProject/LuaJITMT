#!/bin/sh
# Run the Lua-defined M5 empty-hash table insertion guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RIn -- 'nextnode(' "$ROOT/tests" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw nextnode() test traversal is forbidden; use lj_tab_nextnode_acq()' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_tab_emptyhash
