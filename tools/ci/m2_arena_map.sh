#!/bin/sh
# Build and run the arena mmap scaffold test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t_arena_map

"$CC" $CFLAGS -I"$ROOT/src" \
  "$ROOT/tests/t-arena-map.c" "$ROOT/src/lj_arena.c" \
  "$ROOT/src/lj_prng.c" -o "$OUT.map"
"$OUT.map"

"$CC" $CFLAGS -I"$ROOT/src" \
  "$ROOT/tests/t-arena-huge.c" "$ROOT/src/lj_arena.c" \
  "$ROOT/src/lj_prng.c" -o "$OUT.huge"
"$OUT.huge"
