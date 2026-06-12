#!/bin/sh
# Build and run M5 metatable negative-cache policy guards.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t-nomm-cache

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-nomm-cache.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

if rg -n -- "->nomm\\s*\\|=" "$ROOT/src/lj_meta.c"; then
  echo "guardrail: runtime metamethod misses must not set nomm bits" >&2
  exit 1
fi

clears=$(rg -n "mt->nomm = 0;.*stale metamethod miss" \
  "$ROOT/src/lj_api.c" "$ROOT/src/lib_base.c" | wc -l)
if [ "$clears" -ne 2 ]; then
  echo "guardrail: C API and base setmetatable must clear installed mt nomm" >&2
  exit 1
fi

if ! awk '
  /\.ffunc_2 setmetatable/ { inff = 1; next }
  inff && /mov byte TAB:RA->nomm, 0/ { clear = 1 }
  inff && /mov TAB:RB->metatable, TAB:RA/ {
    if (!clear) exit 1
    exit 0
  }
  END { if (!clear) exit 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 setmetatable fast path must clear installed mt nomm" >&2
  exit 1
fi

if ! awk '
  /static void LJ_FASTCALL recff_setmetatable/ { infn = 1; next }
  infn && /IRFL_TAB_NOMM/ { clear = 1 }
  infn && /IRFL_TAB_META/ { meta = 1 }
  infn && /^}/ { exit(clear && meta ? 0 : 1) }
  END { if (!clear || !meta) exit 1 }
' "$ROOT/src/lj_ffrecord.c"; then
  echo "guardrail: recorder setmetatable must clear installed mt nomm" >&2
  exit 1
fi

echo "M5 nomm cache tests passed"
