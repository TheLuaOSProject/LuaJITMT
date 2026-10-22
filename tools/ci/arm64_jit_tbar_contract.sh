#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_tbar_contract SKIP: requires native macOS arm64"
  exit 0
fi

cc=${CC:-clang}
arch=${LJ_ARM64_TBAR_ARCH:-arm64}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=${LJ_ARM64_TBAR_ARCHIVE:-$root/src/libluajit.a}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-jit-tbar.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_ARM64_EMIT_TEST_HELPERS'

test "$(lipo -archs "$archive")" = "$arch"
nm "$archive" | grep ' T _lj_asm_arm64_tbar_test$' >/dev/null
sed -n '/^static void asm_tbar(/,/^}/p' \
  "$root/src/lj_asm_arm64.h" >"$tmpdir/lowering"
grep -F 'IRCALL_lj_gc_tbar_trace_g' "$tmpdir/lowering" >/dev/null
if grep -E 'grayagain|offsetof\(GCtab, (marked|gclist)\)' \
     "$tmpdir/lowering" >/dev/null; then
  echo "ARM64 TBAR still modifies the obsolete color-GC frontier" >&2
  exit 1
fi

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch "$arch" \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-tbar.c" \
  "$root/tests/t-arm64-jit-tbar-call.S" "$archive" -lm -pthread \
  -o "$tmpdir/t-arm64-jit-tbar"
perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
  30 "$tmpdir/t-arm64-jit-tbar" "$tmpdir/emitted.bin"

"$cc" -arch "$arch" -mmacosx-version-min="$minver" -x assembler \
  -c /dev/null -o "$tmpdir/empty.o"
ld -r -arch "$arch" -o "$tmpdir/emitted.o" "$tmpdir/empty.o" \
  -sectcreate __TEXT __text "$tmpdir/emitted.bin"
otool -tvV "$tmpdir/emitted.o" >"$tmpdir/emitted.disasm"
grep -E '[[:space:]]mov[[:space:]]+x0, x22' \
  "$tmpdir/emitted.disasm" >/dev/null
grep -E '[[:space:]]add[[:space:]]+x2, x25' \
  "$tmpdir/emitted.disasm" >/dev/null
grep -E '[[:space:]]bl(raaz|r)?[[:space:]]' \
  "$tmpdir/emitted.disasm" >/dev/null
echo "arm64_jit_tbar_contract OK: production $arch TBAR lowering executes GC2 barriers"
