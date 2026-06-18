#!/bin/sh
# Guard M7 FFI ctype-table RCU growth and SMR retirement.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-ffi-ctype-tab-retire

for needle in \
  'typedef struct CTypeTab' \
  'CTypeTab *tabh' \
  'CTypeTab *retiredtab' \
  'ctype_tabh_acq(CTState *cts)' \
  'ctype_tab_acq(CTState *cts)' \
  'la_loadptr_acq((void *const *)&cts->tabh)' \
  'ctype_tab_grow_l(lua_State *L, CTState *cts, CTypeID id)' \
  'la_casptr((void **)&cts->tabh, &expect, newh' \
  'ctype_tab_retire(cts, oldh)' \
  'lj_ctype_reclaim_retired(global_State *g, uint64_t completed_epoch)' \
  'lj_ctype_reclaim_retired(g, epoch)' \
  'lj_gc2_markmem(g, ctype_tabh_acq(cts))' \
  'lj_gc_arena_markmem(g, ctype_tabh_acq(cts))' \
  'gc2_paranoia_checkmem(g, ctype_tabh_acq(cts), "ctype table")'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI ctype table-retirement marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_mem_growvec\(L, cts->tab|lj_mem_freevec\(cts->g, cts->tab|la_storeptr_rel\(\(void \*\*\)&cts->tab,' \
    "$ROOT/src/lj_ctype.c"; then
  echo "guardrail: ctype table growth must publish/retire, not realloc/free in place" >&2
  exit 1
fi

if awk '
  /typedef struct CTState/ { inside = 1 }
  inside && /^} CTState;/ { inside = 0 }
  inside && /(CType \*tab|MSize sizetab)/ { print; bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_ctype.h"; then
  echo "guardrail: CTState must not keep ctype table mirror fields" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" XCFLAGS="-DLUAJIT_CTYPE_CHECK_ANCHOR" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-ffi-ctype-tab-retire.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

echo "M7 FFI ctype table-retirement guard passed"
