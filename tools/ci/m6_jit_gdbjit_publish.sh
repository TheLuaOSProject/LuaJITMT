#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(eo|head|prev|next).*(->[[:space:]]*entry[.](next_entry|prev_entry)|->[[:space:]]*(next_entry|prev_entry))|__jit_debug_descriptor[.](first_entry|relevant_entry|action_flag)[[:space:]]*=' \
  "$ROOT/src/lj_gdbjit.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GDBJIT descriptor link access is forbidden; use gdbjit_* helpers' >&2
  exit 1
fi

if hits=$(grep -nE -- '__sync_|__atomic_' "$ROOT/src/lj_gdbjit.c" || true); \
    [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'GDBJIT synchronization must route through lj_atomic.h helpers' >&2
  exit 1
fi

make -C "$ROOT/src" clean
status=0
make -C "$ROOT/src" XCFLAGS="${XCFLAGS:-} -DLUAJIT_USE_GDBJIT" || status=$?
make -C "$ROOT/src" clean
exit "$status"
