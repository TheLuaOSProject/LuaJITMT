#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(grep -nE 'ps->[[:space:]]*abort[[:space:]]*=|if[[:space:]]*\([[:space:]]*ps->[[:space:]]*abort[[:space:]]*\)' \
  "$ROOT/src/lj_profile.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'profile timer abort flag must use la_load32_acq/la_store32_rel' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_state_owner
