#!/bin/sh
# Build and run the arena bitmap scaffold test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
OUT=${TMPDIR:-/tmp}/lj_t_arena_bitmap

"$CC" -std=gnu99 -O2 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/tests/t-arena-bitmap.c" "$ROOT/src/lj_arena.c" -o "$OUT"
"$OUT"
