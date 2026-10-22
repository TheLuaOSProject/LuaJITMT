#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_phi_spill_contract SKIP: requires native macOS arm64"
  exit 0
fi
cc=${CC:-clang}
arch=${LJ_ARM64_PHI_ARCH:-arm64}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=${LJ_ARM64_PHI_ARCHIVE:-$root/src/libluajit.a}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-jit-phi-spill.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_ARM64_EMIT_TEST_HELPERS'
test "$(lipo -archs "$archive")" = "$arch"
nm "$archive" | grep ' T _lj_asm_arm64_phi_spill_test$' >/dev/null
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch "$arch" \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-phi-spill.c" \
  "$root/tests/t-arm64-jit-phi-spill-call.S" "$archive" -lm -pthread \
  -o "$tmpdir/t-arm64-jit-phi-spill"
perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
  30 "$tmpdir/t-arm64-jit-phi-spill" "$tmpdir/emitted.bin"
"$cc" -arch "$arch" -mmacosx-version-min="$minver" -x assembler \
  -c /dev/null -o "$tmpdir/empty.o"
ld -r -arch "$arch" -o "$tmpdir/emitted.o" "$tmpdir/empty.o" \
  -sectcreate __TEXT __text "$tmpdir/emitted.bin"
otool -tvV "$tmpdir/emitted.o" >"$tmpdir/emitted.disasm"
# Ordinary same-block loads must still pair; the PHI-boundary stores must not.
grep -E '[[:space:]]ldp[[:space:]]+d' "$tmpdir/emitted.disasm" >/dev/null
if grep -E '[[:space:]]stp[[:space:]]+d' "$tmpdir/emitted.disasm" >/dev/null; then
  echo "ARM64 PHI copy still paired across the body entry" >&2
  exit 1
fi
echo "arm64_jit_phi_spill_contract OK: production $arch loop/PHI boundary executes"
