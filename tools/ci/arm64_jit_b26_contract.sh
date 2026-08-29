#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_b26_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
asm_object=$root/src/lj_asm.o
admit_source=$root/src/lj_asm_arm64_admit.h
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-b26.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-b26
fixture_arm64e=$tmpdir/t-arm64-jit-b26-arm64e.o
audit_object=$tmpdir/lj_asm-arm64e.o
encoder_region=$tmpdir/encoder-region.txt
fixup_region=$tmpdir/tail-fixup-region.txt
prep_region=$tmpdir/tail-prep-region.txt
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'

test -f "$archive" || {
  echo "ARM64 B26 contract requires an existing experimental build" >&2
  exit 1
}
test -f "$asm_object" && test "$asm_object" -nt "$admit_source" || {
  echo "ARM64 B26 object is stale relative to its admission source" >&2
  exit 1
}
test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_asm_arm64_b26_encode$' >/dev/null || {
  echo "experimental archive lacks the public ARM64 B26 encoder" >&2
  exit 1
}

grep -F 'LJ_FUNC int lj_asm_arm64_b26_encode(uintptr_t source,' \
  "$root/src/lj_asm.h" >/dev/null
grep -E '^#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
  "$root/src/lj_arch.h" >/dev/null
grep -F 'sh "$root/tools/ci/arm64_jit_b26_contract.sh"' \
  "$root/tools/ci/arm64_jit_fail_closed_gate.sh" >/dev/null

awk '/^int lj_asm_arm64_b26_encode\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$admit_source" >"$encoder_region"
test -s "$encoder_region"
for required in \
  'source == 0 || target == 0 || insp == NULL' \
  '((source | target) & 3u) != 0' \
  'if (target >= source)' \
  'distance = target - source;' \
  'distance > UINT32_C(0x07fffffc)' \
  'distance = source - target;' \
  'distance > UINT32_C(0x08000000)' \
  '(0u - (uint32_t)(distance >> 2)) & UINT32_C(0x03ffffff)' \
  '*insp = (MCode)(A64I_B | immediate);'; do
  grep -F "$required" "$encoder_region" >/dev/null || {
    echo "ARM64 B26 encoder invariant changed: $required" >&2
    exit 1
  }
done
if grep -E 'ptrdiff_t|(^|[^[:alnum:]_])intptr_t|A64F_S26|A64F_S_OK|A64I_LE|lj_bswap|\(MCode \*\)' \
     "$encoder_region" >/dev/null; then
  echo "ARM64 B26 encoder regained pointer/signed or endian conversion" >&2
  exit 1
fi

sed -n '/^static MCode \*asm_tail_fixup(/,/^}/p' \
  "$root/src/lj_asm_arm64.h" >"$fixup_region"
sed -n '/^static void asm_tail_prep(/,/^}/p' \
  "$root/src/lj_asm_arm64.h" >"$prep_region"
test -s "$fixup_region" && test -s "$prep_region"
test "$(grep -Fc 'lj_asm_arm64_b26_encode(' "$fixup_region")" = 1
test "$(grep -Fc 'lj_asm_arm64_b26_encode(' "$prep_region")" = 2
for required in \
  'MCode *certified_parent_mcode)' \
  '!as->parent || certified_parent_mcode == NULL' \
  'target = certified_parent_mcode;' \
  'branchpc = mcp;' \
  '(uintptr_t)(void *)mcp' \
  '(uintptr_t)(void *)target' \
  '} else if (lnk) {' \
  'lj_trace_err(as->J, LJ_TRERR_MCODEOV);' \
  'A64I_LDRx' \
  'A64I_BR_AUTH' \
  'return branchpc;'; do
  grep -F "$required" "$fixup_region" >/dev/null || {
    echo "ARM64 tail fixup lost B26/fallback invariant: $required" >&2
    exit 1
  }
done
if grep -F 'traceref(' "$fixup_region" >/dev/null; then
  echo "ARM64 tail fixup regained an uncertified trace-slot dereference" >&2
  exit 1
fi
for required in \
  '(uintptr_t)(void *)p' \
  '(uintptr_t)(void *)(p-1)' \
  'without an SP adjustment or at p with one' \
  'p--;'; do
  grep -F "$required" "$prep_region" >/dev/null || {
    echo "ARM64 tail reservation lost B26 invariant: $required" >&2
    exit 1
  }
done
if grep -E 'target[[:space:]]*-[[:space:]]*(mcp|p)|A64F_S26|A64F_S_OK|\(p\+1\)' \
     "$fixup_region" "$prep_region" >/dev/null; then
  echo "ARM64 tail path regained unchecked or stale B26 arithmetic" >&2
  exit 1
fi

fixture_source=$root/tests/t-arm64-jit-b26.c
for required in \
  'source-B26_MIN_BYTES' \
  'source+B26_MAX_BYTES' \
  'source-B26_MIN_BYTES-4u' \
  'source+B26_MAX_BYTES+4u' \
  'distance <= B26_MIN_BYTES+16u' \
  'distance <= B26_MAX_BYTES+16u' \
  'for (bit = 0; bit < 32; bit++)' \
  'decoded != target' \
  'source+lowbits, source' \
  'source, source+lowbits' \
  'lj_asm_arm64_b26_encode(source, source, NULL)' \
  'expect_reject(4u, highest)' \
  'expect_reject(highest, 4u)' \
  'UINT32_C(0xdeadbeef)'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 B26 boundary/mutation coverage changed: $required" >&2
    exit 1
  }
done

# Compile both the implementation and fixture for arm64e so pointer
# authentication ABI configuration cannot silently change their types.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/src/lj_asm.c" -o "$audit_object"
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64e \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_arm64e"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$fixture_source" "$archive" -lm -pthread -o "$fixture"
"$fixture"

echo "arm64_jit_b26_contract OK: exact signed B26 limits, all single-bit decode mutations and certified linked-tail rejection paths verified"
