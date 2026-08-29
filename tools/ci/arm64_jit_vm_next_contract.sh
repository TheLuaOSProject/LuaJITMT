#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_vm_next_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
base_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TAB_TEST_HELPERS'
pauth_xcflags="$base_xcflags -DLUAJIT_ENABLE_CET_BR"
safe_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT'
restore_xcflags=${LJ_VM_NEXT_RESTORE_XCFLAGS:-$safe_xcflags}
native_target_flags='-arch arm64'
pauth_target_flags='-arch arm64e -mbranch-protection=bti'

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS="$native_target_flags" \
        XCFLAGS="$restore_xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS="$native_target_flags" \
        XCFLAGS="$restore_xcflags" >/dev/null 2>&1 || status=1
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

if test "${LJ_TEST_DISABLE_RUN_LOCK:-0}" != 1 &&
   test "${LJ_TEST_RUN_LOCK_HELD:-0}" != 1; then
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 vm_next contract lock timed out: $lock_dir" >&2
      exit 2
    fi
    sleep 0.2
  done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-vm-next.XXXXXX")
vm_source=$root/src/vm_arm64.dasc
vm_object=$root/src/lj_vm.o
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-vm-next-forward.c
cc=${CC:-clang}
restore_needed=1

for source_pattern in \
  'arm64_vm_gl_u32_acq NEXT_TMP1w, NEXT_TMP0, mt_active' \
  'arm64_vm_gl_u32_acq NEXT_TMP1w, NEXT_TMP0, mt_entering' \
  'arm64_vm_tab_array_flags_acq NEXT_TMP0w, NEXT_TMP1' \
  'tst NEXT_TMP0w, #TABARRAY_FLAG_RETIRING' \
  'movz NEXT_TMP2, #LJ_TFORWARD_BITS&0xffff' \
  'arm64_vm_tab_node_flags_acq NEXT_TMP0w, NEXT_TMP1' \
  'tst NEXT_TMP0w, #TABNODE_FLAG_RETIRING' \
  'movz NEXT_TMP0, #LJ_TKEYLOCK_BITS&0xffff' \
  'stp NEXT_TMP2, NEXT_TMP3, [sp]' \
  'bl extern lj_tab_vmnext_forward' \
  'add CARG1, sp, #16' \
  'ret_auth'
do
  grep -F "$source_pattern" "$vm_source" >/dev/null || {
    echo "ARM64 vm_next source lost: $source_pattern" >&2
    exit 1
  }
done

extract_vm_next() {
  object=$1
  output=$2
  otool -tvV "$object" | awk '
    /^_lj_vm_next:/ { copy=1 }
    copy && seen && /^_[^:]*:/ { exit }
    copy { print; seen=1 }
  ' >"$output"
  test -s "$output"
}

inspect_vm_next() {
  label=$1
  expect_pauth=$2
  disasm=$tmpdir/$label.vm-next.disasm
  relocs=$tmpdir/$label.vm.relocs
  extract_vm_next "$vm_object" "$disasm"
  otool -rv "$vm_object" >"$relocs"

  ldar_count=$(grep -Ec '[[:space:]]ldar[[:space:]]' "$disasm")
  test "$ldar_count" -eq 12 || {
    echo "ARM64 $label vm_next emitted $ldar_count LDARs, expected 12" >&2
    exit 1
  }
  for object_pattern in \
    'ldar[[:space:]]+w3, \[x14\]' \
    'ldar[[:space:]]+x9, \[x14\]' \
    'ldar[[:space:]]+x8, \[x14\]' \
    'ldar[[:space:]]+x10, \[x8\]' \
    'ldar[[:space:]]+x11, \[x14\]' \
    'stp[[:space:]]+x10, x11, \[sp\]' \
    'mov[[:space:]]+w1, w0' \
    'add[[:space:]]+x0, sp, #0x10'
  do
    grep -Eq "$object_pattern" "$disasm" || {
      echo "ARM64 $label vm_next object lost: $object_pattern" >&2
      exit 1
    }
  done
  grep -F '_lj_tab_vmnext_forward' "$relocs" >/dev/null || {
    echo "ARM64 $label vm_next lacks its helper relocation" >&2
    exit 1
  }
  if grep -Eq 'ldr[[:space:]]+(w3|x9), \[x0, #0x(10|28|30)\]' \
      "$disasm"; then
    echo "ARM64 $label vm_next regressed to raw table-vector loads" >&2
    exit 1
  fi

  if test "$expect_pauth" = 1; then
    grep -Eq '^0*[[:xdigit:]]+[[:space:]]+bti[[:space:]]+c$' \
      "$disasm" || {
      echo "ARM64e vm_next lacks BTI C entry" >&2
      exit 1
    }
    test "$(grep -Ec '[[:space:]]pacibsp$' "$disasm")" -eq 1
    test "$(grep -Ec '[[:space:]]retab$' "$disasm")" -eq 1
  else
    if grep -Eq '[[:space:]](pacibsp|retab)$' "$disasm"; then
      echo "ordinary ARM64 vm_next unexpectedly emitted PAUTH instructions" >&2
      exit 1
    fi
  fi
}

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS="$native_target_flags" \
    XCFLAGS="$base_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS="$native_target_flags" \
    XCFLAGS="$base_xcflags"
test "$(lipo -archs "$vm_object")" = arm64
test "$(lipo -archs "$archive")" = arm64
inspect_vm_next arm64 0

native_fixture=$tmpdir/t-arm64-vm-next-forward
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" \
  -DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL \
  -DLUA_USE_ASSERT -DLJ_TAB_TEST_HELPERS -I"$root/src" \
  "$fixture_source" "$archive" \
  -lm -ldl -pthread -o "$native_fixture"
"$native_fixture"

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS="$pauth_target_flags" \
    XCFLAGS="$pauth_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS="$pauth_target_flags" \
    XCFLAGS="$pauth_xcflags"
test "$(lipo -archs "$vm_object")" = arm64e
test "$(lipo -archs "$archive")" = arm64e
inspect_vm_next arm64e 1

pauth_fixture=$tmpdir/t-arm64e-vm-next-forward
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  -DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL \
  -DLUA_USE_ASSERT -DLJ_TAB_TEST_HELPERS -DLUAJIT_ENABLE_CET_BR \
  -I"$root/src" \
  "$fixture_source" "$archive" -lm -ldl -pthread -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
"$pauth_fixture"

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS="$native_target_flags" \
    XCFLAGS="$restore_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS="$native_target_flags" \
    XCFLAGS="$restore_xcflags"
restore_needed=0

echo "arm64_jit_vm_next_contract OK: ARM64/arm64e fast, forwarded, retiring and KEYLOCK snapshots passed"
