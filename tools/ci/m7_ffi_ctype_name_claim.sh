#!/bin/sh
# Guard M7 FFI ctype duplicate-name publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-ffi-ctype-name-claim

for needle in \
  'ctype_hash_findname(CTState *cts, CTypeID id, GCstr *name,' \
  'lj_ctype_addname_unique(CTState *cts, CType *ct, CTypeID id,' \
  'ctype_hash_findname(cts, head, name, tmask)' \
  'ctype_abandon(cts, id)' \
  'return winner;  /* 11.2 named ctype duplicate winner. */' \
  'return id;  /* 11.2 CAS-prepend named ctype publication. */' \
  'lj_ctype_addname_unique(cp->cts, ct, sid,' \
  'lj_ctype_addname_unique(cp->cts, ct, constid, CPNS_DEFAULT)' \
  'lj_ctype_addname_unique(cp->cts, ct, id, CPNS_DEFAULT)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI ctype name-claim marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_ctype_addname\(cp->cts' "$ROOT/src/lj_cparse.c"; then
  echo "guardrail: parser global name publication must use duplicate-aware claim helper" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-ffi-ctype-name-claim.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdef-dup-stack.lua" \
  "${LJ_M7_FFI_CDEF_DUP_ROUNDS:-30}" \
  "${LJ_M7_FFI_CDEF_DUP_ITERS:-200}"

echo "M7 FFI ctype name-claim guard passed"
