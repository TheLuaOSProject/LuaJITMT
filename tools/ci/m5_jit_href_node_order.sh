#!/bin/sh
# Run the Lua-defined x64 JIT HREF table node/hmask load-order guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'offsetof\\((GCtab|Node),[[:space:]]*(node|next)\\)' \
    "$ROOT/src/lj_asm_x86.h" | \
    grep -vE -- 'asm_href_(tab_node|node_next)_acq' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw x86/x64 JIT HREF table-link emitters are forbidden; use asm_href_*_acq helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_jit_href_node_order
