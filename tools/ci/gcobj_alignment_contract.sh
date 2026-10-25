#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-gcobj-alignment.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
# This header-only regression must actually instrument union member access.
# Concrete tables retain their stronger alignment and existing pair protocol.
${CC:-clang} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=undefined -fno-sanitize-recover=all \
  -DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -I"$root/src" \
  "$root/tests/t-gcobj-alignment.c" -o "$tmpdir/alignment"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=0 "$tmpdir/alignment"
