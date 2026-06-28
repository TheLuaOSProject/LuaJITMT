#!/bin/sh
# Run the M5 jit.profile native-state and callback ownership guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if ! awk '
  /^static void jit_profile_callback\(/ { in_fn = 1; found = 1 }
  in_fn && /lj_tv_load_acq\(&cbtv, tv\)/ { saw_acq = 1 }
  in_fn && /setfuncV\(L2, L2->top\+\+, funcV\(&cbtv\)\)/ { saw_func = 1 }
  in_fn && /^}/ { in_fn = 0 }
  END { exit(found && saw_acq && saw_func ? 0 : 1) }
' "$ROOT/src/lib_jit.c"; then
  printf '%s\n' 'jit_profile_callback() must snapshot the hidden callback slot before calling' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_profile_stop_native
