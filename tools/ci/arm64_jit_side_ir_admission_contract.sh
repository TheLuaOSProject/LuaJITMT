#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_side_ir_admission_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-side-adaptation.c
asm_source=$root/src/lj_asm.c
admit_source=$root/src/lj_asm_arm64_admit.h
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-side-admission.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
fixture=$tmpdir/t-arm64-jit-side-adaptation

if test ! -f "$archive" || test ! -f "$fixture_source"; then
  echo "ARM64 side admission requires an experimental archive and native fixture" >&2
  exit 1
fi
if test "$(lipo -archs "$archive")" != arm64; then
  echo "ARM64 side admission archive must contain exactly arm64" >&2
  exit 1
fi

xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
# The native test observes the exact patched edge, successful root-entry
# handoffs, native exits, and a later guarded type mismatch.
for symbol in _lj_trace_test_exit_calls _lj_trace_test_root_entry_publishes \
    _lj_trace_test_root_entry_successes _lj_trace_test_root_entry_cleanups; do
  nm "$archive" | grep " T ${symbol}$" >/dev/null || {
    echo "experimental archive lacks native side witness: $symbol" >&2
    exit 1
  }
done

# Keep both independent admission passes on the actual recorder/allocator path.
grep -F 'lj_asm_arm64_side_scalar_ir_admit(' "$asm_source" >/dev/null
grep -F 'lj_asm_arm64_side_scalar_postra_admit(' "$admit_source" >/dev/null
if grep -E 'ARM64_SIDE_(K_ADDEND|R_[A-Z]+|SEMANTIC_NINS)|LJArm64SideShape|lj_asm_arm64_side_shape' \
     "$asm_source" "$admit_source"; then
  echo "obsolete exact ARM64 side-shape admission remains in production" >&2
  exit 1
fi

# shellcheck disable=SC2086 # xcflags intentionally expands to compiler arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" "$fixture_source" "$archive" -lm -pthread -o "$fixture"
"$fixture"

echo "arm64_jit_side_ir_admission_contract OK: native scalar side adaptation and guarded refusal verified"
