#!/bin/sh
# Run the M6 JIT local-cell operation guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
REC="$ROOT/src/lj_record.c"

if ! awk '
  /^static TRef rec_upvalue\(/ {
    in_fn = 1
    found = 1
    saw_cell_bypass = 0
    saw_self_shortcut = 0
  }
  in_fn && /proto_celluv\(J->pt\)/ { saw_cell_bypass = 1 }
  in_fn && /funcV\(uvval\(uvp\)\) == J->fn/ {
    saw_self_shortcut = 1
    if (!saw_cell_bypass)
      exit 1
  }
  in_fn && /^}/ {
    if (!(saw_cell_bypass && saw_self_shortcut))
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$REC"; then
  printf '%s\n' 'rec_upvalue must keep local-cell function upvalues on the explicit UREFC/ULOAD path before any immutable self shortcut' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_jit_cell_ops
