#!/bin/sh
# Build and run M5 direct registry root publication guards.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t-registry-root

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-registry-root.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

if ! awk '
  /if \(idx == LUA_REGISTRYINDEX\)/ { inreg = 1; next }
  inreg && /copyTVrel\(L, o, f\)/ { copy = 1 }
  inreg && /lj_gc_barrierroot\(L, f\)/ { barrier = 1 }
  inreg && /\} else \{/ { inreg = 0 }
  END { exit(copy && barrier ? 0 : 1) }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: direct registry writes need barrierroot + copyTVrel" >&2
  exit 1
fi

echo "M5 registry root tests passed"
