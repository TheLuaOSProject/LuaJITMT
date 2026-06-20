#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -RIn -- 'gcnext(' "$ROOT/src" "$ROOT/tests" | \
    grep -vE -- '/src/lj_obj[.]h:[0-9]+:#define gcnext' || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw gcnext() traversal is forbidden; use lj_obj_gcw_acq()' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_jit_alloc_account
