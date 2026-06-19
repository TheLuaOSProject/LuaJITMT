#!/bin/sh
# Build and run M5 table KEYLOCK lookup filtering guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-keylock-lookup

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-keylock-lookup.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'tab_key_islocked(cTValue *key)' \
  'tab_key_retry_once(cTValue *key, int *retry)' \
  'if (tab_key_retry_once(&nk, &retry))' \
  'if (tab_key_islocked(&key))' \
  'tviskeylock(&key)' \
  'tviskeylock(&out[0])'
do
  if ! rg -F -q "$needle" "$ROOT/src" "$ROOT/tests/t-tab-keylock-lookup.c"; then
    echo "guardrail: missing table KEYLOCK lookup marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'm5_tab_keylock_lookup.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: table KEYLOCK lookup guard is not wired into M5 aggregate" >&2
  exit 1
fi

echo "M5 table KEYLOCK lookup filtering tests passed"
