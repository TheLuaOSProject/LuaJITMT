#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_oserr_unwind_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=
jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
base_flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUAJIT_UNWIND_EXTERNAL -DLUA_USE_ASSERT -DLJ_OSERR_TEST_UNWIND_CLOBBER'
default_restore_flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
restore_flags=${LJ_OSERR_RESTORE_XCFLAGS:-$default_restore_flags}
fixture_source=$root/tests/t-oserr-unwind-landing.c

cleanup() {
  saved_status=$?
  restore_status=0
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$restore_flags" >/dev/null 2>&1 || restore_status=$?
    if test "$restore_status" = 0; then
      env MACOSX_DEPLOYMENT_TARGET="$minver" \
        make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
          XCFLAGS="$restore_flags" >/dev/null 2>&1 || restore_status=$?
    fi
  fi
  if test -n "$tmpdir"; then
    rm -rf "$tmpdir"
  fi
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test "$saved_status" = 0 && test "$restore_status" != 0; then
    echo "ARM64 OS-error unwind contract could not restore arm64 build" >&2
    saved_status=$restore_status
  fi
  exit "$saved_status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if test "${LJ_TEST_DISABLE_RUN_LOCK:-0}" != 1 && \
   test "${LJ_TEST_RUN_LOCK_HELD:-0}" != 1; then
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  lock_announced=0
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 OS-error unwind contract lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then
        echo "owner:" >&2
        sed -n '1,20p' "$lock_dir/owner" >&2 || true
      fi
      exit 2
    fi
    if test "$lock_announced" = 0; then
      echo "waiting for Lua test runner lock: $lock_dir" >&2
      lock_announced=1
    fi
    sleep 0.2
  done
  lock_held=1
  {
    printf 'time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'cmd=%s\n' "$0"
  } >"$lock_dir/owner" 2>/dev/null || true
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-oserr.XXXXXX")
restore_needed=1

check_arm64_disassembly() {
  tag=$1
  object=$2
  region=$tmpdir/vm-unwind-os-$tag.txt
  otool -tvV "$object" | \
    sed -n '/_lj_vm_unwind_os_eh:/,/^_lj_vm_unwind_c:/p' | \
    tr '\t' ' ' >"$region"
  test -s "$region"
  for required in \
    'sub sp, sp, #0x300' \
    'add sp, sp, #0x300' \
    'stp x0, x1, [sp]' \
    'stp x2, x3, [sp, #0x10]' \
    'stp x4, x5, [sp, #0x20]' \
    'stp x6, x7, [sp, #0x30]' \
    'stp x8, x9, [sp, #0x40]' \
    'stp x10, x11, [sp, #0x50]' \
    'stp x12, x13, [sp, #0x60]' \
    'stp x14, x15, [sp, #0x70]' \
    'stp x16, x17, [sp, #0x80]' \
    'stp x19, x20, [sp, #0x90]' \
    'stp x21, x22, [sp, #0xa0]' \
    'stp x23, x24, [sp, #0xb0]' \
    'stp x25, x26, [sp, #0xc0]' \
    'stp x27, x29, [sp, #0xd0]' \
    'mov x19, x28' \
    'ldr x0, [x19]' \
    'ldr x30, [x19, #0x8]' \
    'ldr x28, [x19, #0x10]' \
    'ldp x27, x29, [sp, #0xd0]' \
    'ldp x25, x26, [sp, #0xc0]' \
    'ldp x23, x24, [sp, #0xb0]' \
    'ldp x21, x22, [sp, #0xa0]' \
    'ldp x19, x20, [sp, #0x90]' \
    'ldp x16, x17, [sp, #0x80]' \
    'ldp x14, x15, [sp, #0x70]' \
    'ldp x12, x13, [sp, #0x60]' \
    'ldp x10, x11, [sp, #0x50]' \
    'ldp x8, x9, [sp, #0x40]' \
    'ldp x6, x7, [sp, #0x30]' \
    'ldp x4, x5, [sp, #0x20]' \
    'ldp x2, x3, [sp, #0x10]' \
    'ldp x0, x1, [sp]' \
    'mrs x16, NZCV' \
    'msr NZCV, x16' \
    'mrs x16, FPCR' \
    'msr FPCR, x16' \
    'mrs x17, FPSR' \
    'msr FPSR, x17'; do
    grep -F "$required" "$region" >/dev/null || {
      echo "$tag OS-error trampoline lost: $required" >&2
      exit 1
    }
  done
  test "$(grep -Ec '[[:space:]]stp[[:space:]]+x' "$region")" = 14
  test "$(grep -Ec '[[:space:]]ldp[[:space:]]+x' "$region")" = 14
  test "$(grep -c 'stp[[:space:]]*q' "$region")" = 16
  test "$(grep -c 'ldp[[:space:]]*q' "$region")" = 16
  if grep -E '(^|[^[:alnum:]_])x18([^[:alnum:]_]|$)' "$region" >/dev/null; then
    echo "$tag OS-error trampoline touched reserved x18" >&2
    exit 1
  fi
  nm -u "$object" | grep '_lj_err_restore_os$' >/dev/null
  if test "$tag" = arm64e; then
    grep -F 'bti' "$region" >/dev/null
    grep -F 'retab' "$region" >/dev/null
  else
    grep -E '[[:space:]]ret$' "$region" >/dev/null
  fi
}

run_arm_case() {
  tag=$1
  target_flags=$2
  xcflags=$3
  expected_arch=$4
  fixture=$tmpdir/t-oserr-unwind-$tag

  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" clean TARGET_SYS=Darwin \
      TARGET_FLAGS="$target_flags" XCFLAGS="$xcflags"
  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" -j"$jobs" TARGET_SYS=Darwin \
      TARGET_FLAGS="$target_flags" XCFLAGS="$xcflags"
  test "$(lipo -archs "$root/src/libluajit.a")" = "$expected_arch"
  test "$(lipo -archs "$root/src/lj_vm.o")" = "$expected_arch"

  # shellcheck disable=SC2086 # Target/compiler flag groups are intentional.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
    "$fixture_source" "$root/src/libluajit.a" -lm -ldl -pthread \
    -o "$fixture"
  "$fixture"
  check_arm64_disassembly "$tag" "$root/src/lj_vm.o"
}

run_arm_case arm64 '-arch arm64' "$base_flags" arm64
run_arm_case arm64e '-arch arm64e -mbranch-protection=bti' \
  "$base_flags -DLUAJIT_ENABLE_CET_BR" arm64e
otool -hv "$tmpdir/t-oserr-unwind-arm64e" | \
  grep -E 'ARM64[[:space:]]+E' >/dev/null

x64_flags='-DLUAJIT_UNWIND_EXTERNAL -DLUA_USE_ASSERT -DLJ_OSERR_TEST_UNWIND_CLOBBER'
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_SYS=Darwin \
    TARGET_FLAGS='-arch x86_64' XCFLAGS="$x64_flags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_SYS=Darwin \
    TARGET_FLAGS='-arch x86_64' XCFLAGS="$x64_flags"
test "$(lipo -archs "$root/src/libluajit.a")" = x86_64
# shellcheck disable=SC2086 # Compiler flag group is intentional.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch x86_64 -mcx16 \
  -mmacosx-version-min="$minver" $x64_flags -I"$root/src" \
  "$fixture_source" "$root/src/libluajit.a" -lm -ldl -pthread \
  -o "$tmpdir/t-oserr-unwind-x86_64"
arch -x86_64 "$tmpdir/t-oserr-unwind-x86_64"

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$restore_flags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$restore_flags"
restore_needed=0

echo "arm64_oserr_unwind_contract OK: arm64, arm64e/BTI and x86_64 preserved C/fast-function landing errno"
