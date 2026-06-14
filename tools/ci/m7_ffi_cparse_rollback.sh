#!/bin/sh
# Guard M7 FFI cparser rollback without CTState top/hash rewind.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-ffi-cparse-rollback
ANCHOR_OUT=${TMPDIR:-/tmp}/lj_t-ffi-cparse-rollback-anchor

for needle in \
  'typedef struct CPRollback CPRollback' \
  'CPRollback *rollback' \
  'CTypeID starttop' \
  'uint8_t newtype' \
  'CPAlloc *newct' \
  'ctype_isabandoned(info)' \
  'cp_rollback_log(CPState *cp, CTypeID id)' \
  'cp_ctype_publish(CPState *cp, CTypeID id, CType *src)' \
  'cp_ctype_setsib(CPState *cp, CTypeID id, CTypeID sib)' \
  'cp_ctype_new(CPState *cp, CType **ctp)' \
  'cp_ctype_intern(CPState *cp, CTInfo info, CTSize size)' \
  'lj_ctype_intern_new_l(cp->L, cp->cts' \
  'cp_ctype_abandon(CPState *cp)' \
  'cp_ctype_publish(cp, rb->id, ct)' \
  'cp_ctype_publish(cp, fieldid, ct)' \
  'for (ca = cp->newct; ca != NULL; ca = ca->next)' \
  'cp_rollback_restore(CPState *cp)' \
  'if (errcode)' \
  'cp_rollback_restore(cp)' \
  'ffi_checkctype_layout_lock(lua_State *L, CTState *cts,' \
  'LJLIB_CF(ffi_new)' \
  'ffi.new waits out parser rollback.' \
  'LJLIB_CF(ffi_typeinfo)' \
  'Snapshot ctype while parser rollback cannot mutate layout.' \
  'layout reader waits out parser rollback' \
  'lj_ctype_parse_unlock(cts);' \
  'direct ctype reader observed failed cdef rollback state' \
  'ffi.typeinfo observed failed cdef rollback state' \
  'ffi.new observed failed cdef rollback state' \
  'cdata __index observed failed cdef rollback state' \
  'cdata __newindex observed failed cdef rollback state' \
  'cdata numeric __index observed failed cdef rollback state' \
  'cdata pointer add observed failed cdef rollback state' \
  'cdata pointer diff observed failed cdef rollback state' \
  'enum string cast observed failed cdef rollback state' \
  'ffi.C observed failed cdef rollback constant' \
  'direct ctype/typeinfo/new/field/numeric/ptrarith/namespace readers wait out rollback' \
  'cdata string-key readers wait out parser rollback' \
  'cdata numeric-key readers wait out parser rollback' \
  'cdata pointer arithmetic readers wait out parser rollback' \
  'enum string readers wait out parser rollback' \
  'ffi.C namespace readers wait out parser rollback' \
  'cdata recorder field reader waits out parser rollback' \
  'cdata recorder numeric-key reader waits out parser rollback' \
  'cdata recorder pointer arithmetic waits out rollback' \
  'recorder enum string reader waits out parser rollback' \
  'recorder ffi.C namespace reader waits out parser rollback' \
  'if (errcode || cp.newtype)' \
  'ctype_top_acq(cp->cts)'
do
  if ! rg -F -q "$needle" "$ROOT/src" \
      "$ROOT/tests/t-ffi-cparse-rollback-reader.lua"; then
    echo "guardrail: missing FFI cparser rollback marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'LJ_CTYPE_SAVE|LJ_CTYPE_RESTORE|memcpy\(\(cts\)->hash|newtop > oldtop|oldtop = cp\.cts->top' \
    "$ROOT/src/lj_ctype.h" "$ROOT/src/lj_cparse.c" "$ROOT/src/lj_crecord.c"; then
  echo "guardrail: cparser rollback must not rewind CTState top/hash" >&2
  exit 1
fi

if awk '
  /cp_ctype_new\(CPState \*cp, CType \*\*ctp\)/ { helper = 1 }
  helper && /^}/ { helper = 0; next }
  !helper && /lj_ctype_new_l\(cp->L, cp->cts/ { print; bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_cparse.c"; then
  echo "guardrail: parser allocations must route through cp_ctype_new" >&2
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

if rg -n 'cp_ctype_mut\(cp, [^)]+\)->sib' "$ROOT/src/lj_cparse.c"; then
  echo "guardrail: parser sib writes must publish through cp_ctype_setsib" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(ffi_new\)/ {
    infn = 1
  }
  infn && /ffi_checkctype\(L, cts, NULL\)/ {
    badplain = 1
  }
  infn && /ffi_checkctype_layout_lock\(L, cts, NULL\)/ && !lock {
    lock = NR
  }
  infn && /11\.2: ffi\.new waits out parser rollback/ {
    unlock = NR
  }
  infn && /lj_cdata_newx_l\(L, cts, id, sz, info\)/ {
    alloc = NR
  }
  infn && /^}/ {
    infn = 0
  }
  END { exit !badplain && lock && unlock && alloc &&
	      lock < unlock && unlock < alloc ? 0 : 1 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: ffi.new must validate layout under parser rollback token before allocation" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-ffi-cparse-rollback.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cparse-rollback-reader.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-ffi-cparse-rollback-reader.lua"

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" XCFLAGS="-DLUAJIT_CTYPE_CHECK_ANCHOR" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-ffi-cparse-rollback.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$ANCHOR_OUT"
timeout 20s "$ANCHOR_OUT"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cparse-rollback-reader.lua"
"$ROOT/src/luajit" "$ROOT/tests/t-ffi-cparse-rollback-reader.lua"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdef-token.lua" 2 20

echo "M7 FFI cparser rollback guard passed"
