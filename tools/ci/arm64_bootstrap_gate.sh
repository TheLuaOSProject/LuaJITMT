#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_bootstrap_gate; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_bootstrap_gate
LJ_TEST_RUN_LOCK_HELD=1
export LJ_TEST_RUN_LOCK_HELD

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT'
luajit=$root/src/luajit
dylib=$root/src/libluajit.so
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
lua_path="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;"

lj_ci_set_restore '-arch arm64' "$xcflags"
lj_ci_build_profile arm64 '-arch arm64' "$xcflags" bootstrap

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

legacy_fixture=$root/src/t-legacy-bytecode-cells
# shellcheck disable=SC2086 # Match the archive's interpreter layout.
${CC:-clang} -std=gnu11 -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-legacy-bytecode-cells.c" "$archive" -lm -pthread \
  -o "$legacy_fixture"
perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
  120 "$legacy_fixture" "$root/tests/fixtures"
rm -f "$legacy_fixture"

LJ_ARM64_VM_OBJECT="$vm_object" \
LJ_ARM64_DISPATCH_OBJECT="$root/src/lj_dispatch.o" \
  sh "$root/tools/ci/arm64_tg_dispatch_contract.sh"
LJ_ARM64_VM_OBJECT="$vm_object" \
  sh "$root/tools/ci/arm64_root_publication_contract.sh"
LJ_ARM64_VM_OBJECT="$vm_object" LJ_ARM64_ARCHIVE="$archive" \
  sh "$root/tools/ci/arm64_tmpbuf_contract.sh"
LJ_ARM64_VM_OBJECT="$vm_object" LJ_ARM64_ARCHIVE="$archive" \
  sh "$root/tools/ci/arm64_vm_safepoint_contract.sh"
LJ_ARM64_VM_OBJECT="$vm_object" LJ_ARM64_ARCHIVE="$archive" \
  sh "$root/tools/ci/arm64_pcall_anchor_contract.sh"

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
    m5_arm64_root_publication_runtime m5_arm64_safepoint_runtime \
    m5_arm64_pcall_anchor_runtime m5_arm64_capi_meta_roots \
    m5_arm64_tmpbuf_runtime \
    m4_posix_signal_arm64_gate m3_darwin_gc2_runtime \
    m7_ffi_callback_arm64_lifecycle m7_ffi_errno_preservation

(
  cd "$root/tests/stock/test"
  env LUA_PATH="$lua_path" "$luajit" test.lua --quiet
)

run_lua -joff "$root/tests/t-threading-api.lua"
run_lua -joff "$root/tests/t-threading-hooks.lua"
run_lua -joff "$root/tests/t-threading-coroutine.lua"
run_lua -joff "$root/tests/t-upvalue-identity.lua"
env LJ_TEST_ROOT="$root" LUA_PATH="$lua_path" \
  "$luajit" -joff "$root/tests/t-jit-cell-loops.lua"
run_lua -joff "$root/tests/t-ffi-callback-runtime.lua" 4 80

LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
LJ_CALLBACK_LIFECYCLE_RESTORE_XCFLAGS="$xcflags" \
  sh "$root/tools/ci/arm64e_ffi_callback_lifecycle_contract.sh"

LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_ffi_aggregate_contract.sh"

LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
LJ_OSERR_RESTORE_XCFLAGS="$xcflags" \
  sh "$root/tools/ci/arm64_oserr_unwind_contract.sh"

LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
LJ_ARM64_GC2_RESTORE_XCFLAGS="$xcflags" \
  sh "$root/tools/ci/arm64_gc2_interp_hard_check_contract.sh"

lj_ci_restore '-arch arm64' "$xcflags"
echo "arm64_bootstrap_gate OK: assert interpreter, GC2 hard assist, stock, TG hooks, coroutine/dead-resume and FFI callbacks"
