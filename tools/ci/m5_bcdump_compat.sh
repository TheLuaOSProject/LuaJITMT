#!/bin/sh
# Build and guard M5 bytecode dump compatibility.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-bcdump-compat

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-bcdump-compat.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  'BCDUMP_VERSION_LEGACY' \
  'BCDUMP_VERSION_TRANS' \
  'BCDUMP_VERSION_LOCKLESS' \
  'PROTO2_LEGACYUV' \
  'PROTO2_CELLUV' \
  'proto_setlegacyuv' \
  'proto_setcelluv' \
  'bcread_verify_bytecode' \
  'bcread_uv_haslocal' \
  'BCREAD_CELL_CNEW' \
  'bcread_version(ls) != BCDUMP_VERSION_LOCKLESS && op >= BC_CNEW' \
  'cellops & BCREAD_CELL_CNEW' \
  'bcwrite_has_legacyuv'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_bcdump.h" "$ROOT/src/lj_obj.h" \
    "$ROOT/src/lj_bcread.c" "$ROOT/src/lj_bcwrite.c"
  then
    echo "guardrail: missing bytecode compatibility marker: $needle" >&2
    exit 1
  fi
done

if rg -n '#if[[:space:]]+LJ_MT|#ifdef[[:space:]]+LJ_MT|LUAJIT_THREADSAFE' \
  "$ROOT/src/lj_bcread.c" "$ROOT/src/lj_bcwrite.c" "$ROOT/src/lj_bcdump.h" \
  "$ROOT/src/lj_obj.h"
then
  echo "guardrail: bytecode compatibility must not be hidden behind LJ_MT" >&2
  exit 1
fi

echo "M5 bytecode dump compatibility tests passed"
