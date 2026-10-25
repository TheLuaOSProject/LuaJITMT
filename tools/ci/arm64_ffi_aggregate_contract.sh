#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_ffi_aggregate_contract SKIP: requires native macOS arm64"
  exit 0
fi

cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
test "$(lipo -archs "$archive")" = arm64
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-ffi-aggregate.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUA_USE_ASSERT -DLJ_CCALL_TEST_HELPERS'
if nm "$archive" | grep -E ' T _lj_trace_hot$' >/dev/null; then
  xcflags="$xcflags -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL"
else
  xcflags="$xcflags -DLUAJIT_DISABLE_JIT"
fi

# Replace only the FFI object when linking this fixture, exposing its private
# classifier for the deterministic field-snapshot wait. The checked-out build
# and its executable remain untouched. Match the archive's JIT layout.
# shellcheck disable=SC2086
"$cc" -O2 -g -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -c "$root/src/lj_ccall.c" -o "$tmpdir/lj_ccall.o"
"$cc" -O2 -arch arm64 -mmacosx-version-min="$minver" -dynamiclib \
  "$root/tests/t-arm64-ffi-aggregate-lib.c" -o "$tmpdir/aggregate.dylib"
# shellcheck disable=SC2086
"$cc" -O2 -g -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -I"$root/tests" "$root/tests/t-arm64-ffi-aggregate.c" \
  "$tmpdir/lj_ccall.o" "$archive" -o "$tmpdir/aggregate"

env LUA_PATH="$root/src/?.lua;;" \
  LJ_ARM64_AGGREGATE_LIB="$tmpdir/aggregate.dylib" \
  perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
  30 "$tmpdir/aggregate"
echo "arm64_ffi_aggregate_contract OK: native aggregate/vararg ABI, parser waits and automatic GC errno"
