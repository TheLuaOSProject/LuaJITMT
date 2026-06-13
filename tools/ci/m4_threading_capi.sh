#!/bin/sh
# Run focused M4 public C threading API tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
TMP=${TMPDIR:-/tmp}

for needle in \
  'mt_shutdown' \
  'la_futex_wait(&g->mt_live' \
  'la_futex_wake(&g->mt_live' \
  'lj_safepoint_ack(tg->thread_L)' \
  'lua_close returned before attached thread detached'
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_threading.c" "$ROOT/src/lj_obj.h" "$ROOT/src/lj_tg.c" "$ROOT/tests/t-threading-capi.c"; then
    echo "guardrail: C attach shutdown wait missing marker: $needle" >&2
    exit 1
  fi
done

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

OUT="$TMP/lj_t-threading-capi"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-threading-capi.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

echo "M4 public C threading API tests passed"
