#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_fail_closed_gate SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0

cleanup_lock() {
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
}

while ! mkdir "$lock_dir" 2>/dev/null; do
  sleep 0.2
done
lock_held=1
trap cleanup_lock EXIT HUP INT TERM
printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_ARM64_EMIT_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS'
luajit=$root/src/luajit
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
lua_path="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;"

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" XCFLAGS="$xcflags" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

for artifact in "$luajit" "$archive" "$vm_object"; do
  test "$(lipo -archs "$artifact")" = arm64 || {
    echo "fail-closed artifact is not thin arm64: $artifact" >&2
    exit 1
  }
done

if ! nm "$archive" | grep -E ' T _lj_trace_(hot|ins)$' >/dev/null; then
  echo "fail-closed archive does not contain the JIT recorder" >&2
  exit 1
fi
if ! nm "$root/src/lj_mcode.o" | \
     grep '_pthread_jit_write_protect_np' >/dev/null; then
  echo "desktop ARM64 mcode object did not compile the MAP_JIT write gate" >&2
  exit 1
fi

if grep -n 'GL_J(trace)' "$root/src/vm_arm64.dasc" >/dev/null; then
  echo "ARM64 VM still contains a raw jit_State trace-table dereference" >&2
  exit 1
fi

LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_emitter_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/jit_hotcount_generation_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/jit_recorder_safepoint_contract.sh"

env LUA_PATH="$lua_path" "$luajit" -e '
assert(jit.status() == true, "experimental build did not admit JIT APIs")
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local sum = 0
for round = 1, 128 do
  for i = 1, 4096 do sum = sum + i end
  collectgarbage("step", 64)
end
assert(sum == 128 * 4096 * 4097 / 2)
assert(util.traceinfo(1) == nil, "fail-closed ARM64 build published trace 1")
'

LJ_ARM64_SAFEPOINT_SOURCE_ONLY=1 \
  sh "$root/tools/ci/arm64_vm_safepoint_contract.sh"

env MACOSX_DEPLOYMENT_TARGET="$minver" LUA_PATH="$lua_path" \
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  "$luajit" "$root/tools/test.lua" \
    m5_arm64_jit_fail_closed_safepoint_runtime

echo "arm64_jit_fail_closed_gate OK: JIT linked, zero traces, interpreter safepoints sound"
