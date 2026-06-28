#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for required in \
  'static void dispatch_update_wait_no_l(void)' \
  '(void)lj_thr_sleep_ns(NULL, 1000000);' \
  'test_dispatch_update_regular_wait(L);' \
  'test_dispatch_update_async_return(L);'
do
  if ! grep -qF "$required" "$ROOT/src/lj_dispatch.c" \
     && ! grep -qF "$required" "$ROOT/tests/t-safepoint-handshake.c"; then
    printf '%s\n' "dispatch update wait is missing: $required" >&2
    exit 1
  fi
done

if hits=$(awk '
  /^void LJ_FASTCALL lj_dispatch_update\(global_State \*g, int nolock\)/ {
    inside = 1
  }
  inside && /la_cpu_pause[[:space:]]*\(/ {
    print FILENAME ":" FNR ":" $0
  }
  inside && /^}/ {
    inside = 0
  }
' "$ROOT/src/lj_dispatch.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'dispatch update token contention must wait in native slices, not spin on la_cpu_pause()' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_dispatch_redispatch
