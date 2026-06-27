#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
for helper in jit_token_acq jit_token_rel jit_token_cas; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for the JIT recorder token" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*jit_token|&[[:space:]]*[^)]*->[[:space:]]*jit_token' \
    "$ROOT"/src/*.c \
    "$ROOT/tests/t-jit-token.c" \
    "$ROOT/tests/t-safepoint-handshake.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw JIT recorder token access is forbidden; use jit_token_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m6_jit_token
