#!/bin/sh
# Build and run the M5 concurrent table protocol model.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16"}
OUT=${TMPDIR:-/tmp}/lj_t-nbtab-model

"$CC" $CFLAGS "$ROOT/tests/t-nbtab-model.c" -o "$OUT"
"$OUT"

echo "M5 nbtab model tests passed"
