#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_pauth_emit_target_contract SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=
jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
base_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" XCFLAGS="$base_xcflags" \
      >/dev/null 2>&1 || status=1
  fi
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test -n "$tmpdir"; then
    rm -rf "$tmpdir"
  fi
  exit "$status"
}
trap cleanup EXIT HUP INT TERM

if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" != 1 &&
   test "${LJ_TEST_RUN_LOCK_HELD:-}" != 1; then
  while ! mkdir "$lock_dir" 2>/dev/null; do sleep 0.2; done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-pauth-emit.XXXXXX")
cc=${CC:-clang}
test_xcflags="$base_xcflags -DLJ_ARM64_PAUTH_EMIT_TEST_HELPERS"
pauth_xcflags="$test_xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
asm_object=$root/src/lj_asm.o
fixture_source=$root/tests/t-arm64-pauth-emit-target.c
native_fixture=$tmpdir/t-arm64-pauth-emit-target
pauth_fixture=$tmpdir/t-arm64-pauth-emit-target-arm64e
native_disasm=$tmpdir/lj-asm-native.disasm
pauth_disasm=$tmpdir/lj-asm-arm64e.disasm
native_direct=$tmpdir/native-direct.txt
native_runtime=$tmpdir/native-runtime.txt
native_indirect=$tmpdir/native-indirect.txt
pauth_direct=$tmpdir/pauth-direct.txt
pauth_runtime=$tmpdir/pauth-runtime.txt
pauth_indirect=$tmpdir/pauth-indirect.txt
pauth_relocs=$tmpdir/lj-asm-arm64e.relocs
restore_needed=1

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" XCFLAGS="$test_xcflags" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$test_xcflags"
test "$(lipo -archs "$archive")" = arm64
for symbol in _lj_asm_arm64_emit_target_direct_test \
  _lj_asm_arm64_emit_target_runtime_test \
  _lj_asm_arm64_emit_target_indirect_bits_test; do
  nm "$asm_object" | grep " T $symbol$" >/dev/null
done
# shellcheck disable=SC2086 # test_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $test_xcflags -I"$root/src" \
  "$fixture_source" "$archive" -lm -pthread -o "$native_fixture"
"$native_fixture"

otool -tvV "$asm_object" >"$native_disasm"
awk '/^_lj_asm_arm64_emit_target_direct_test:/ { copy=1 }
     copy && seen && /^_[^:]*:/ { exit }
     copy { print; seen=1 }' \
  "$native_disasm" >"$native_direct"
awk '/^_lj_asm_arm64_emit_target_runtime_test:/ { copy=1 }
     copy && seen && /^_[^:]*:/ { exit }
     copy { print; seen=1 }' \
  "$native_disasm" >"$native_runtime"
awk '/^_lj_asm_arm64_emit_target_indirect_bits_test:/ { copy=1 }
     copy && seen && /^_[^:]*:/ { exit }
     copy { print; seen=1 }' \
  "$native_disasm" >"$native_indirect"
test -s "$native_direct" && test -s "$native_runtime" &&
test -s "$native_indirect"
if grep -E '[[:space:]](aut|pac)[a-z0-9]*[[:space:]]' \
     "$native_direct" "$native_runtime" "$native_indirect" >/dev/null; then
  echo "ordinary ARM64 target materialization unexpectedly contains PAUTH" >&2
  exit 1
fi

env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
otool -hv "$asm_object" | grep -E 'ARM64[[:space:]]+E' >/dev/null
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" "$fixture_source" "$archive" \
  -lm -pthread -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
"$pauth_fixture"

otool -tvV "$asm_object" >"$pauth_disasm"
awk '/^_lj_asm_arm64_emit_target_direct_test:/ { copy=1 }
     copy && seen && /^_[^:]*:/ { exit }
     copy { print; seen=1 }' \
  "$pauth_disasm" >"$pauth_direct"
awk '/^_lj_asm_arm64_emit_target_runtime_test:/ { copy=1 }
     copy && seen && /^_[^:]*:/ { exit }
     copy { print; seen=1 }' \
  "$pauth_disasm" >"$pauth_runtime"
awk '/^_lj_asm_arm64_emit_target_indirect_bits_test:/ { copy=1 }
     copy && seen && /^_[^:]*:/ { exit }
     copy { print; seen=1 }' \
  "$pauth_disasm" >"$pauth_indirect"
test -s "$pauth_direct" && test -s "$pauth_runtime" &&
test -s "$pauth_indirect"
if grep -E '[[:space:]](aut|pac|xpac)[a-z0-9]*[[:space:]]' \
     "$pauth_direct" "$pauth_indirect" >/dev/null; then
  echo "ARM64e raw label or retained indirect bits contain PAUTH" >&2
  exit 1
fi
grep -E '[[:space:]]auti[a-z0-9]*[[:space:]]+x[0-9]+' \
  "$pauth_runtime" >/dev/null
grep -E 'xpaci[[:space:]]+x[0-9]+' "$pauth_runtime" >/dev/null

# The real VM-label callsites use raw PAGE21/PAGOF12 materialization. The
# executable assertion above separately pins the untouched signed far-call
# constant which is later loaded for generated BLRAAZ.
otool -rv "$asm_object" >"$pauth_relocs"
for label in _lj_vm_exit_handler _lj_vm_exit_interp; do
  test "$(grep -c " $label$" "$pauth_relocs")" -ge 2
  if grep " $label$" "$pauth_relocs" | \
     grep -Ev ' (PAGE21|PAGOF12) ' >/dev/null; then
    echo "ARM64e VM label uses non-raw relocation: $label" >&2
    exit 1
  fi
done

grep -F 'ptrauth_auth_data(ptrauth_nop_cast(char *, target)' \
  "$root/src/lj_emit_arm64.h" >/dev/null
test "$(grep -Fc 'emit_asmlabel_addr(lj_vm_exit_handler)' \
  "$root/src/lj_asm_arm64.h")" = 1
test "$(grep -Fc 'emit_asmlabel_addr(lj_vm_exit_interp)' \
  "$root/src/lj_asm_arm64.h")" = 2
if grep -F 'emit_asmfunc_addr((ASMFunction)lj_vm_' \
     "$root/src/lj_asm_arm64.h" >/dev/null; then
  echo "raw VM assembler label still enters function-pointer authentication" >&2
  exit 1
fi

echo "arm64_pauth_emit_target_contract OK: direct and signed runtime targets normalize on ARM64e"
