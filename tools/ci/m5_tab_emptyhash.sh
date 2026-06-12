#!/bin/sh
# Build and run M5 empty-hash table insertion guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-emptyhash

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-emptyhash.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

if rg -n '\|\| t->hmask == 0' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: lj_tab_newkey must branch before hashing empty hash part" >&2
  exit 1
fi

if ! awk '
  /TValue \*lj_tab_newkey/ { infn = 1; next }
  infn && /if \(t->hmask == 0\)/ { branch = NR }
  infn && /hashkey\(t, key\)/ && !hash { hash = NR }
  infn && /nodebase != &G\(L\)->nilnode/ { assert_nilnode = 1 }
  infn && /^}/ {
    exit(branch && hash && branch < hash && assert_nilnode ? 0 : 1)
  }
  END { if (!branch || !hash || branch > hash || !assert_nilnode) exit 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: lj_tab_newkey must rehash/retry before first hashkey on empty hash part" >&2
  exit 1
fi

echo "M5 empty-hash table insertion tests passed"
