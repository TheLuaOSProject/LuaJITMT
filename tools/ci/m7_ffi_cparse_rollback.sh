#!/bin/sh
# Guard M7 FFI cparser rollback without CTState top/hash rewind.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-ffi-cparse-rollback

for needle in \
  'typedef struct CPRollback CPRollback' \
  'CPRollback *rollback' \
  'CTypeID starttop' \
  'uint8_t newtype' \
  'ctype_isabandoned(info)' \
  'cp_rollback_log(CPState *cp, CTypeID id)' \
  'cp_ctype_new(CPState *cp, CType **ctp)' \
  'cp_ctype_abandon(CPState *cp)' \
  'cp_rollback_restore(CPState *cp)' \
  'if (errcode)' \
  'cp_rollback_restore(cp)' \
  'if (errcode || cp.newtype)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
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

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-ffi-cparse-rollback.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

echo "M7 FFI cparser rollback guard passed"
