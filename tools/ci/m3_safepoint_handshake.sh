#!/bin/sh
# Run the Lua-defined M3 safepoint handshake guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '->[[:space:]]*next_tg|&[[:alnum:]_]+->[[:space:]]*next_tg|next_tg[[:space:]]*=' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_safepoint.c" \
    "$ROOT/src/lib_threading.c" \
    "$ROOT/src/lj_tg.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw TGState next_tg access is forbidden; use lj_tg_next_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m3_safepoint_handshake
