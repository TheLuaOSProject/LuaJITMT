#!/bin/sh
# Build and run the arena mmap scaffold test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
OUT=${TMPDIR:-/tmp}/lj_t_arena_map

"$CC" -std=gnu99 -O2 -Wall -Wextra -Werror -I"$ROOT/src" \
  "$ROOT/tests/t-arena-map.c" "$ROOT/src/lj_arena.c" \
  "$ROOT/src/lj_prng.c" -o "$OUT"
"$OUT"
