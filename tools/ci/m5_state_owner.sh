#!/bin/sh
# Build and run M5 lua_State owner claim guards.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t-state-owner

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-state-owner.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

if [ "$(rg -n "lj_state_tryclaim\\(" "$ROOT/src/lj_api.c" | wc -l)" -lt 2 ]; then
  echo "guardrail: lua_xmove must claim both foreign states" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(coroutine_status\)/ { infn = 1; next }
  infn && /lj_state_tryclaim\(co/ { claim = 1 }
  infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && drop ? 0 : 1) }
  END { if (!claim || !drop) exit 1 }
' "$ROOT/src/lib_base.c"; then
  echo "guardrail: coroutine.status must claim foreign coroutine state" >&2
  exit 1
fi

echo "M5 lua_State owner tests passed"
