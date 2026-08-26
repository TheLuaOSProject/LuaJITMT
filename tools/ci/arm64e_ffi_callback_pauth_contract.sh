#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64e_ffi_callback_pauth_contract SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=
jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
base_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
test_xcflags="$base_xcflags -DLUAJIT_ENABLE_CET_BR"
native_target_flags='-arch arm64'
pauth_target_flags='-arch arm64e -mbranch-protection=bti'

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS="$native_target_flags" \
        XCFLAGS="$base_xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS="$native_target_flags" \
        XCFLAGS="$base_xcflags" >/dev/null 2>&1 || status=1
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

acquire_lock() {
  if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" = 1 ||
     test "${LJ_TEST_RUN_LOCK_HELD:-}" = 1; then
    return
  fi
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  lock_announced=0
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "ARM64e FFI callback contract lock timed out: $lock_dir" >&2
      exit 2
    fi
    if test "$lock_announced" = 0; then
      echo "waiting for Lua test runner lock: $lock_dir" >&2
      lock_announced=1
    fi
    sleep 0.2
  done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
}

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64e-ffi-callback.XXXXXX")

cc=${CC:-clang}
archive=$root/src/libluajit.a
callback_object=$root/src/lj_ccallback.o
fixture=$tmpdir/t-arm64e-ffi-callback-pauth
macros=$tmpdir/macros.txt
callback_disasm=$tmpdir/lj-ccallback.disasm
ptr2slot_disasm=$tmpdir/ptr2slot.disasm
new_disasm=$tmpdir/new.disasm
restore_needed=1

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS="$pauth_target_flags" \
    XCFLAGS="$test_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS="$pauth_target_flags" \
    XCFLAGS="$test_xcflags"

# shellcheck disable=SC2086 # flag groups intentionally expand to arguments.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $test_xcflags -I"$root/src" \
  -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1'; do
  grep -E "^#define ${setting% *}[[:space:]]+${setting#* }$" \
    "$macros" >/dev/null || {
    echo "ARM64e FFI callback macro mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # flag groups intentionally expand to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $test_xcflags -I"$root/src" \
  "$root/tests/t-arm64e-ffi-callback-pauth.c" "$archive" \
  -lm -pthread -o "$fixture"
otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
"$fixture"

# Pin both pointer-auth operations in the compiled callback implementation.
otool -tvV "$callback_object" >"$callback_disasm"
awk '/^_lj_ccallback_ptr2slot:/ { copy=1 }
     copy && seen && /^_[^:]*:/ { exit }
     copy { print; seen=1 }' "$callback_disasm" >"$ptr2slot_disasm"
awk '/^_lj_ccallback_new_l:/ { copy=1 }
     copy && seen && /^_[^:]*:/ { exit }
     copy { print; seen=1 }' "$callback_disasm" >"$new_disasm"
test -s "$ptr2slot_disasm"
test -s "$new_disasm"
grep -E '[[:space:]]xpaci[[:space:]]+x[0-9]+' "$ptr2slot_disasm" >/dev/null
grep -E '[[:space:]]paci(a|za)[[:space:]]' "$new_disasm" >/dev/null

# The executable fixture inspects every generated slot entry at runtime. Keep
# the production source branch pinned too, so BTI cannot silently become NOP.
grep -F '#if LJ_ABI_BRANCH_TRACK' "$root/src/lj_ccallback.c" >/dev/null
grep -F '*p++ = A64I_BTI_C;' "$root/src/lj_ccallback.c" >/dev/null
grep -F 'return lj_ptr_sign(p, 0);' "$root/src/lj_ccallback.c" >/dev/null
grep -F 'addr = (uintptr_t)lj_ptr_strip(p);' \
  "$root/src/lj_ccallback.c" >/dev/null

# Leave the shared checkout in the ordinary native experimental mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS="$native_target_flags" \
    XCFLAGS="$base_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS="$native_target_flags" \
    XCFLAGS="$base_xcflags"
restore_needed=0

lua_path="$root/tests/lib/?.lua;$root/tests/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;"
env LUA_PATH="$lua_path" "$root/src/luajit" \
  "$root/tests/t-ffi-callback-install.lua" 2 16
env LUA_PATH="$lua_path" "$root/src/luajit" \
  "$root/tests/t-ffi-callback-runtime.lua" 2 32

echo "arm64e_ffi_callback_pauth_contract OK: signed BTI callback create/call/set/free/reuse verified"
