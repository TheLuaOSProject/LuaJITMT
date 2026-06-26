#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

for helper in hookf_load hookf_store hookcount_load hookcstart_load hookcount_store hookcount_setstart hookcount_reset; do
  if ! grep -q "static LJ_AINLINE .* ${helper}" src/lj_obj.h; then
    printf '%s\n' "${helper} helper is required for global hook state" >&2
    exit 1
  fi
done

if hits=$(grep -nE '__atomic_(load|store)_n\(&g->hookf' src/lj_obj.h || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'global hook function publication must use lj_atomic.h helpers' >&2
  exit 1
fi

if hits=$(grep -RInE -- '->hook(f|count|cstart)' src/lj_*.c src/lib_*.c src/lj_*.h 2>/dev/null | \
    grep -Ev '^src/lj_obj\.h:' || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'C-side hook function/count access must use hook* helpers' >&2
  exit 1
fi

locked=$(grep -cF 'lock; dec dword GL:ITYPE->hookcount' src/vm_x64.dasc || true)
if [ "$locked" -lt 2 ]; then
  printf '%s\n' 'x64 VM hookcount dispatch decrements must be locked' >&2
  exit 1
fi
if hits=$(grep -nF 'dec dword GL:ITYPE->hookcount' src/vm_x64.dasc | \
    grep -vF 'lock; dec dword GL:ITYPE->hookcount' || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw x64 VM hookcount decrement is forbidden' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_hook_state_atomic
