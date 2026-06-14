#!/bin/sh
# Guard M7 FFI ctype ticket allocation and duplicate-aware interning.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-ffi-ctype-ticket-intern

for needle in \
  'CTypeTab *tabh' \
  'ctype_top_acq(CTState *cts)' \
  'ctype_top_reserve_l(lua_State *L, CTState *cts, CType **ctp)' \
  'la_cas32(&cts->top, &expect, id+1u, LA_ACQ_REL, LA_ACQ)' \
  'ctype_hash_findtype(CTState *cts, CTypeID id, CTInfo info,' \
  'ctype_hash_try_prepend(CTState *cts, uint32_t h, CType *src,' \
  'ctype_abandon(cts, id)' \
  'lj_ctype_intern_new_l(lua_State *L, CTState *cts' \
  'if (newp) *newp = 1' \
  'cp_ctype_intern(CPState *cp, CTInfo info, CTSize size)' \
  'cp_ctype_publish(CPState *cp, CTypeID id, CType *src)' \
  'lj_cdata_newref_l(lua_State *L, CTState *cts, const void *p,' \
  'lj_ctype_intern_l(L, cts, CTINFO_REF(id), CTSIZE_PTR)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI ctype ticket/intern marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'cts->top[[:space:]]*=|cts->top\+\+|\+\+cts->top|id = cts->top' \
    "$ROOT/src/lj_ctype.c" "$ROOT/src/lj_cparse.c"; then
  echo "guardrail: ctype top must advance through ticket reservation" >&2
  exit 1
fi

if awk '
  /cp_ctype_intern\(CPState \*cp, CTInfo info, CTSize size\)/ { helper = 1 }
  helper && /^}/ { helper = 0; next }
  !helper && /lj_ctype_intern_l\(cp->L, cp->cts/ { print; bad = 1 }
  !helper && /lj_ctype_intern_new_l\(cp->L, cp->cts/ { print; bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_cparse.c"; then
  echo "guardrail: parser intern allocations must route through cp_ctype_intern" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-ffi-ctype-ticket-intern.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-ctype-intern-race.lua" \
  "${LJ_M7_FFI_INTERN_THREADS:-6}" \
  "${LJ_M7_FFI_INTERN_SHAPES:-48}" \
  "${LJ_M7_FFI_INTERN_ROUNDS:-4}"

echo "M7 FFI ctype ticket/intern guard passed"
