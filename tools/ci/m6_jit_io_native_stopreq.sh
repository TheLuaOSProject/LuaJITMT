#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

for required in \
  'static int io_native_setvbuf(lua_State *L, FILE *fp, int mode, size_t size)' \
  'io_native_setvbuf(L, fp, opt, sz)' \
  'static void io_native_clearerr(lua_State *L, FILE *fp)' \
  'static int io_native_ferror(lua_State *L, FILE *fp)' \
  'static int io_native_ungetc(lua_State *L, int c, FILE *fp)' \
  'io_native_clearerr(L, fp)' \
  'io_native_ferror(L, fp)' \
  'io_native_ungetc(L, c, fp)'
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

if hits=$(awk '
  /^static (void|int) io_native_(clearerr|ferror|ungetc)\(/ {
    in_wrapper = 1
  }
  !in_wrapper && /(^|[^[:alnum:]_])(clearerr|ferror|ungetc)[[:space:]]*\(/ {
    print FILENAME ":" FNR ":" $0
  }
  in_wrapper && /^}/ {
    in_wrapper = 0
  }
' "$ROOT/src/lib_io.c"); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'stdio state probes must go through native-state wrappers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_jit_io_native_stopreq
