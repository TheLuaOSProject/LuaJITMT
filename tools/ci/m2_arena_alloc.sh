#!/bin/sh
# Build and run the arena bump allocator scaffold test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t_arena_alloc

"$CC" $CFLAGS -I"$ROOT/src" \
  "$ROOT/tests/t-arena-alloc.c" "$ROOT/src/lj_arena.c" \
  "$ROOT/src/lj_prng.c" -o "$OUT.alloc"
"$OUT.alloc"

"$CC" $CFLAGS -I"$ROOT/src" \
  "$ROOT/tests/t-arena-realloc.c" "$ROOT/src/lj_arena.c" \
  "$ROOT/src/lj_prng.c" -o "$OUT.realloc"
"$OUT.realloc"

"$CC" $CFLAGS -I"$ROOT/src" \
  "$ROOT/tests/t-arena-allocf.c" "$ROOT/src/lj_arena.c" \
  "$ROOT/src/lj_prng.c" -o "$OUT.allocf"
"$OUT.allocf"
