#!/bin/sh
# Run the Lua-defined M5 reentrant libc error-string guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(grep -RInE -- 'strerror[[:space:]]*\(' "$ROOT"/src/*.c "$ROOT"/src/*.h | \
    grep -v "$ROOT/src/lj_err.c:" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw production strerror() use is forbidden; use lj_err_strerrno()' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_libc_error_reentrant
