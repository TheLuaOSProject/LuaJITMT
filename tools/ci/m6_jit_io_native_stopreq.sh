#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for required in \
  'static int io_native_setvbuf(lua_State *L, FILE *fp, int mode, size_t size)' \
  'io_native_setvbuf(L, fp, opt, sz)'
do
  if ! grep -qF "$required" "$ROOT/src/lib_io.c"; then
    printf '%s\n' "interpreter IO native-state wrapper is missing: $required" >&2
    exit 1
  fi
done

if hits=$(awk '
  /^LJLIB_CF\(io_method_setvbuf\)/ {
    in_fn = 1
  }
  in_fn && /setvbuf[[:space:]]*\(/ && !/io_native_setvbuf[[:space:]]*\(/ {
    print FILENAME ":" FNR ":" $0
  }
  in_fn && /^}/ {
    in_fn = 0
  }
' "$ROOT/src/lib_io.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'file:setvbuf() must go through the native-state wrapper' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_jit_io_native_stopreq
