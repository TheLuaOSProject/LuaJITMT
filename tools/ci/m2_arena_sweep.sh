#!/bin/sh
# Build and run the owner-local arena sweep scaffold test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t_arena_sweep

"$CC" $CFLAGS -I"$ROOT/src" \
  "$ROOT/tests/t-arena-sweep.c" "$ROOT/src/lj_arena.c" \
  "$ROOT/src/lj_prng.c" -o "$OUT"
"$OUT"
