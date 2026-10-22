#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_numeric_spills_contract SKIP: requires native macOS arm64"
  exit 0
fi
cc=${CC:-clang}
arch=${LJ_ARM64_NUMERIC_SPILLS_ARCH:-arm64}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=${LJ_ARM64_NUMERIC_SPILLS_ARCHIVE:-$root/src/libluajit.a}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-numeric-spills.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
case "$arch" in
  arm64) target_flags= ;;
  arm64e) target_flags=-mbranch-protection=bti ;;
  *) echo "unsupported numeric spill contract architecture: $arch" >&2; exit 1 ;;
esac
test "$(lipo -archs "$archive")" = "$arch"
for symbol in lj_asm_arm64_ir_admit lj_asm_arm64_postra_admit lj_trace_test_root_entry_publishes; do
  nm "$archive" | grep " T _$symbol\$" >/dev/null
done
# shellcheck disable=SC2086 # flag lists intentionally expand to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch "$arch" $target_flags \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-numeric-spills.c" "$archive" -lm -pthread \
  -o "$tmpdir/t-arm64-jit-numeric-spills"
perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
  60 "$tmpdir/t-arm64-jit-numeric-spills" \
  "$root/tests/t-arm64-jit-numeric-spills.lua"
echo "arm64_jit_numeric_spills_contract OK: $arch aligned NUM spans, PHI copies/reloads, FMA machine code, exact multi-result exits, profile/STOPREQ, GC and flush verified"
