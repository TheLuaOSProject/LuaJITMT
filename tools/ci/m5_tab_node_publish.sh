#!/bin/sh
# Run the Lua-defined M5 table hash-vector publication guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(awk '
  /hashgcref/ { inhash = 1 }
  inhash && /gcrefu[[:space:]]*\(/ { print FILENAME ":" FNR ":" $0 }
  inhash && $0 !~ /\\$/ { inhash = 0 }
' "$ROOT/src/lj_tab.h"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'table GC-key hash helpers must use gcrefu_acq()' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_tab_node_publish
