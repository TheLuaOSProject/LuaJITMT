#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_root_entry_contract SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
cleanup() {
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}

while ! mkdir "$lock_dir" 2>/dev/null; do sleep 0.2; done
lock_held=1
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-root-entry.XXXXXX")
trap cleanup EXIT HUP INT TERM
printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
archive=$root/src/libluajit.a
fixture_obj=$tmpdir/t-arm64-jit-root-entry.o
fixture=$tmpdir/t-arm64-jit-root-entry
disasm=$tmpdir/fixture.disasm
abi_region=$tmpdir/abi-region.txt
helper_region=$tmpdir/helper-region.txt

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" XCFLAGS="$xcflags" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_trace_enter_root$' >/dev/null

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/tests/t-arm64-jit-root-entry.c" -o "$fixture_obj"
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-root-entry.c" "$archive" -lm -pthread -o "$fixture"
"$fixture"

# Prove the checked-in caller consumes the 16-byte non-HFA result in x0/x1:
# x6 (the seventh argument) is preserved, the helper has a direct BR26 call,
# x1 is stored as target, and x0 is returned without a hidden x8 sret pointer.
otool -tvV "$fixture_obj" >"$disasm"
awk '/^_lj_test_root_entry_abi_probe:/ { copy=1 }
     copy { print }
     copy && /^_main:/ { exit }' "$disasm" >"$abi_region"
grep -E 'mov[[:space:]]+x19, x6' "$abi_region" >/dev/null
grep -E 'str[[:space:]]+x1, \[x19\]' "$abi_region" >/dev/null
grep -E 'ret$' "$abi_region" >/dev/null
if grep -E '(^|[[:space:],])x8([[:space:],]|$)' "$abi_region" >/dev/null; then
  echo "root-entry ABI probe unexpectedly uses hidden-result register x8" >&2
  exit 1
fi
otool -rv "$fixture_obj" | \
  grep '^00000010 .* BR26 .* _lj_trace_enter_root$' >/dev/null

# Keep the semantic ordering reviewable independently of compiler scheduling.
awk '/^lj_trace_enter_root\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$helper_region"
test -s "$helper_region"
line_of() { grep -n "$1" "$helper_region" | sed -n "${2:-1}p" | cut -d: -f1; }
gate1=$(line_of 'lj_gc2_jit_entry_open(g)' 1)
publish=$(line_of 'lj_tg_store_jit_base(tg, base)' 1)
fence=$(line_of 'la_fence_seq()' 1)
gate2=$(line_of 'lj_gc2_jit_entry_open(g)' 2)
slot1=$(line_of 'trace_root_entry_slot_acq(J, traceno' 1)
mcode=$(line_of 'trace_mcode_acq(T)' 1)
slot2=$(line_of 'trace_root_entry_slot_acq(J, traceno' 2)
tmpbuf=$(line_of 'setsbufL(&tg->tmpbuf, L)' 1)
cleanup_line=$(line_of 'lj_tg_store_jit_base(tg, NULL)' 1)
test "$gate1" -lt "$publish" && test "$publish" -lt "$fence" &&
test "$fence" -lt "$gate2" && test "$gate2" -lt "$slot1" &&
test "$slot1" -lt "$mcode" && test "$mcode" -lt "$slot2" &&
test "$slot2" -lt "$tmpbuf" && test "$tmpbuf" -lt "$cleanup_line"
test "$(grep -c 'lj_tg_store_jit_base(tg, NULL)' "$helper_region")" = 1
for required in trace_runnable_acq trace_root_acq trace_startpc_acq \
  trace_startins_acq trace_szmcode_acq trace_mcauth_acq lj_ptr_strip; do
  grep "$required" "$helper_region" >/dev/null
done
if grep -E 'lj_gc2_smr|lj_mem_|lj_alloc|lj_err_|lj_vmevent|lj_safepoint' \
     "$helper_region" >/dev/null; then
  echo "root-entry helper acquired a forbidden blocking/allocating surface" >&2
  exit 1
fi

env LUA_PATH="$root/src/?.lua;$root/src/jit/?.lua;;" "$root/src/luajit" -e '
  local util = require("jit.util")
  jit.flush(); jit.on(); jit.opt.start("hotloop=1")
  local n = 0
  for r = 1, 64 do for i = 1, 256 do n = n + i end end
  assert(n == 64 * 256 * 257 / 2)
  assert(util.traceinfo(1) == nil, "root-entry tranche enabled recording")
'

echo "arm64_jit_root_entry_contract OK: ABI, rejection, races and fail-closed recorder verified"
