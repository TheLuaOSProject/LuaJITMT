#!/bin/sh
# Run the Lua-defined M5 x64 table next snapshot guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '->[[:space:]]*(node|array|next)\b' \
    "$ROOT/src/vm_x64.dasc" | \
    grep -vE -- 'x64_vm_(tab_node|tab_array|node_next)_acq' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw x64 VM table link loads are forbidden; use x64_vm_*_acq macros' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_x64_table_next_snapshot
