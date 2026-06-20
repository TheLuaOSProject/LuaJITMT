#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

if ! grep -q 'static LJ_AINLINE uint8_t hookmask_load' src/lj_obj.h; then
  printf '%s\n' 'hookmask_load helper is required for C-side hook-mask reads' >&2
  exit 1
fi

if ! grep -q '^|\.macro x64_vm_hookmask_acq' src/vm_x64.dasc; then
  printf '%s\n' 'x64_vm_hookmask_acq macro is required for generated hook-mask reads' >&2
  exit 1
fi
if hits=$(grep -nE 'byte GL:[^ ]+->hookmask' src/vm_x64.dasc | \
    grep -vF 'x64_vm_hookmask_acq' || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'x64 VM hookmask reads must use x64_vm_hookmask_acq' >&2
  exit 1
fi

if hits=$(grep -RInF -- '->hookmask' src/lj_*.c src/lib_*.c src/lj_*.h 2>/dev/null | \
    grep -Ev '^(src/lj_obj\.h:|src/lj_record\.c:[0-9]+:.*&J2G\(J\)->hookmask|src/lj_asm_[^:]+\.h:)' || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'C-side hookmask access must use hookmask_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_hookmask_atomic
