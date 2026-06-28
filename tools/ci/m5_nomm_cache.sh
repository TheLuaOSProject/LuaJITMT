#!/bin/sh
# Guard metatable negative-cache byte access and run the policy fixture.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(awk '
  FILENAME ~ /\/src\/lj_obj\.h$/ { next }
  /^[[:space:]]*(\/\*|\*|\/\/)/ { next }
  /->[[:space:]]*nomm/ {
    print FILENAME ":" FNR ":" $0
  }
' "$ROOT"/src/*.c "$ROOT"/src/*.h "$ROOT"/tests/*.c || true);
then
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'C code must access GCtab.nomm through lj_tab_nomm_*() helpers' >&2
    exit 1
  fi
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_nomm_cache
