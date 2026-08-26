#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_bootstrap_gate SKIP: requires native macOS arm64"
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
      echo "ARM64 bootstrap gate lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then
        echo "owner:" >&2
        cat "$lock_dir/owner" >&2 || true
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
  trap cleanup_lock EXIT HUP INT TERM
  {
    printf 'time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'cmd=%s\n' "$0"
  } >"$lock_dir/owner" 2>/dev/null || true
}

acquire_lock

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT'
luajit=$root/src/luajit
dylib=$root/src/libluajit.so
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
lua_path="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;"

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" XCFLAGS="$xcflags" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

require_thin_arm64() {
  artifact=$1
  if test "$(lipo -archs "$artifact")" != arm64; then
    echo "ARM64 bootstrap artifact is not thin arm64: $artifact" >&2
    lipo -info "$artifact" >&2 || true
    exit 1
  fi
}

for artifact in "$luajit" "$dylib" "$vm_object"; do
  if ! file "$artifact" | grep 'Mach-O 64-bit .* arm64' >/dev/null; then
    echo "ARM64 bootstrap artifact is not an arm64 Mach-O file: $artifact" >&2
    file "$artifact" >&2
    exit 1
  fi
  require_thin_arm64 "$artifact"
done
require_thin_arm64 "$archive"

if nm -u "$luajit" "$dylib" "$vm_object" | \
   grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "ARM64 bootstrap artifact imports a compiler atomic helper" >&2
  nm -u "$luajit" "$dylib" "$vm_object" | \
    grep -E '(__atomic|libatomic)' >&2 || true
  exit 1
fi
if otool -L "$luajit" "$dylib" | grep -i 'libatomic' >/dev/null; then
  echo "ARM64 bootstrap artifact links libatomic" >&2
  exit 1
fi

LJ_ARM64_VM_OBJECT="$vm_object" \
LJ_ARM64_DISPATCH_OBJECT="$root/src/lj_dispatch.o" \
  sh "$root/tools/ci/arm64_tg_dispatch_contract.sh"
LJ_ARM64_VM_OBJECT="$vm_object" \
  sh "$root/tools/ci/arm64_root_publication_contract.sh"
LJ_ARM64_VM_OBJECT="$vm_object" LJ_ARM64_ARCHIVE="$archive" \
  sh "$root/tools/ci/arm64_vm_safepoint_contract.sh"

run_lua() {
  env LUA_PATH="$lua_path" "$luajit" "$@"
}

run_lua -e '
assert(jit.os == "OSX", jit.os)
assert(jit.arch == "arm64", jit.arch)
assert(jit.status() == false, "bootstrap unexpectedly enabled the JIT")
assert(jit.opt == nil, "disabled JIT unexpectedly exposes optimizer controls")
assert(type(require("ffi")) == "table")
'

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  "$luajit" "$root/tools/test.lua" \
    m5_arm64_root_publication_runtime m5_arm64_safepoint_runtime

(
  cd "$root/tests/stock/test"
  env LUA_PATH="$lua_path" "$luajit" test.lua --quiet
)

run_lua -joff "$root/tests/t-threading-api.lua"
run_lua -joff "$root/tests/t-threading-hooks.lua"
run_lua -joff "$root/tests/t-threading-coroutine.lua"
run_lua -joff "$root/tests/t-ffi-callback-runtime.lua" 4 80

echo "arm64_bootstrap_gate OK: assert interpreter, stock, TG hooks, coroutine/dead-resume and FFI callbacks"
