#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_fail_closed_gate; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_fail_closed_gate
LJ_TEST_RUN_LOCK_HELD=1
export LJ_TEST_RUN_LOCK_HELD

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_ARM64_EMIT_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS'
# Nested shared contracts must restore the enclosing archive's complete helper
# profile before the next fixture consumes it.
LJ_TEST_RESTORE_XCFLAGS=$xcflags
LJ_TEST_RESTORE_TARGET_FLAGS='-arch arm64'
LJ_TEST_XCFLAGS=$xcflags
export LJ_TEST_RESTORE_XCFLAGS LJ_TEST_RESTORE_TARGET_FLAGS LJ_TEST_XCFLAGS
luajit=$root/src/luajit
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
lua_path="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/jit/?.lua;;"

lj_ci_set_restore '-arch arm64' "$xcflags"
lj_ci_build_profile arm64 '-arch arm64' "$xcflags" experimental

for artifact in "$luajit" "$archive" "$vm_object"; do
  test "$(lipo -archs "$artifact")" = arm64 || {
    echo "constrained ARM64 artifact is not thin arm64: $artifact" >&2
    exit 1
  }
done

if ! nm "$archive" | grep -E ' T _lj_trace_(hot|ins)$' >/dev/null; then
  echo "constrained ARM64 archive does not contain the JIT recorder" >&2
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

for cell_test in t-jit-cell-cnew-epoch t-jit-cell-pending; do
  cell_fixture=$root/src/$cell_test
  # shellcheck disable=SC2086 # Match the archive's JIT/helper layout.
  ${CC:-clang} -std=gnu11 -O2 -Wall -Wextra -Werror \
    -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
    "$root/tests/$cell_test.c" "$archive" -lm -pthread \
    -o "$cell_fixture"
  "$cell_fixture"
  rm -f "$cell_fixture"
done

legacy_fixture=$root/src/t-legacy-bytecode-cells
# shellcheck disable=SC2086 # Match the archive's JIT/helper layout.
${CC:-clang} -std=gnu11 -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-legacy-bytecode-cells.c" "$archive" -lm -pthread \
  -o "$legacy_fixture"
perl -MTime::HiRes=alarm -e 'alarm shift; exec @ARGV; die "exec: $!\n"' \
  120 "$legacy_fixture" "$root/tests/fixtures"
rm -f "$legacy_fixture"

LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
LJ_ARM64_GC2_RESTORE_XCFLAGS="$xcflags" \
  sh "$root/tools/ci/arm64_gc2_interp_hard_check_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_emitter_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_tbar_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_phi_spill_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_ffi_aggregate_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/jit_hotcount_generation_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/jit_recorder_safepoint_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_ir_admission_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_scalar_capability_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_numeric_spills_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
LJ_ARM64_ROOTED_TABLEN_RESTORE_XCFLAGS="$xcflags" \
  sh "$root/tools/ci/arm64_jit_rooted_tablen_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
LJ_ARM64_ROOTED_READ_RESTORE_XCFLAGS="$xcflags" \
  sh "$root/tools/ci/arm64_jit_rooted_read_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_rooted_store_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_tnew_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_side_ir_admission_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_side_ingress_metadata_contract.sh"
LJ_TEST_ROOT="$root" sh "$root/tools/ci/arm64_jit_b26_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_sealed_root_publish_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_side_asm_consumption_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_first_side_production_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_gdbjit_prepare_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
LJ_VM_NEXT_RESTORE_XCFLAGS="$xcflags" \
  sh "$root/tools/ci/arm64_jit_vm_next_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
LJ_CALLBACK_RESULT_RESTORE_XCFLAGS="$xcflags" \
  sh "$root/tools/ci/arm64_ffi_callback_result_lifetime_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_forl_record_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_funcf_record_contract.sh"
LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  sh "$root/tools/ci/arm64_jit_native_forl_contract.sh"

# The native-loop tranche may be landed independently of this umbrella gate.
# Run its executable proof automatically once the companion contract exists.
native_loop_contract=$root/tools/ci/arm64_jit_native_loop_contract.sh
if test -f "$native_loop_contract"; then
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
    sh "$native_loop_contract"
fi

numeric_loop_contract=$root/tools/ci/arm64_jit_numeric_loop_contract.sh
if test -f "$numeric_loop_contract"; then
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
    sh "$numeric_loop_contract"
fi

pure_numeric_loop_contract=$root/tools/ci/arm64_jit_pure_numeric_loop_contract.sh
if test -f "$pure_numeric_loop_contract"; then
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
    sh "$pure_numeric_loop_contract"
fi

pure_numeric_step_contract=$root/tools/ci/arm64_jit_pure_numeric_step_contract.sh
if test -f "$pure_numeric_step_contract"; then
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
    sh "$pure_numeric_step_contract"
fi

pure_numeric_args_contract=$root/tools/ci/arm64_jit_pure_numeric_args_contract.sh
if test -f "$pure_numeric_args_contract"; then
  for required in \
    'NUMERIC_ARGS_STEP_NUM, NUMERIC_ARGS_STEP_INT, NUMERIC_ARGS_LIMIT_INT, NUMERIC_ARGS_X_INT' \
    'R_STEP, IRCONV_NUM_INT' \
    'limit_source_ref, IRCONV_NUM_INT' \
    'R_X, IRCONV_NUM_INT' \
    'IRCONV_INT_NUM|IRCONV_CHECK' \
    'assert(nscvtf == (step_kind == NUMERIC_ARGS_X_INT ? 2u :' \
    'assert(nfcvtzs == (step_kind == NUMERIC_ARGS_X_INT ? 1u : 0u));' \
    'assert(ncheck == (step_kind == NUMERIC_ARGS_X_INT ? 1u : 0u));' \
    'assert(mcode[shift+17u] == UINT32_C(0x1e620000));' \
    'assert(mcode[shift+31u] == UINT32_C(0x1e7801fc));' \
    'test "$int_x_modes" -eq 12' \
    'test_int_limit_positive_and_guard_exits();' \
    '37 modes/process, 222 default executions'; do
    grep -F "$required" "$pure_numeric_args_contract" >/dev/null || {
      echo "ARM64 umbrella lost NUM/INT widening proof: $required" >&2
      exit 1
    }
  done
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
    sh "$pure_numeric_args_contract"
fi

exit_contract=$root/tools/ci/arm64_jit_exit_contract.sh
if test -f "$exit_contract"; then
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
    sh "$exit_contract"
fi

root_entry_contract=$root/tools/ci/arm64_jit_root_entry_contract.sh
if test -f "$root_entry_contract"; then
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
    sh "$root_entry_contract"
fi

live_flush_contract=$root/tools/ci/arm64_jit_live_flush_reuse_contract.sh
if test -f "$live_flush_contract"; then
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
    sh "$live_flush_contract"
fi

# Nested contracts select their own profiles. Re-establish this gate's exact
# profile before the final runtime and linked-fixture checks.
lj_ci_build_profile arm64 '-arch arm64' "$xcflags" final-runtime
env LUA_PATH="$lua_path" "$luajit" -e '
assert(jit.status() == true, "experimental build did not admit JIT APIs")
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function supported_dynamic_forl(a, b, step)
  local sum = 0
  for i = a, b, step do sum = sum + i end
  return sum
end
local expected = 2048 * 2048
for round = 1, 128 do
  assert(supported_dynamic_forl(1, 4096, 2) == expected)
  collectgarbage("step", 64)
end
assert(util.traceinfo(1) ~= nil,
       "dynamic-step FORL did not publish trace 1")
'

LJ_ARM64_SAFEPOINT_SOURCE_ONLY=1 \
  sh "$root/tools/ci/arm64_vm_safepoint_contract.sh"

env MACOSX_DEPLOYMENT_TARGET="$minver" LUA_PATH="$lua_path" \
  LJ_TEST_ROOT="$root" LJ_TEST_RUN_LOCK_HELD=1 \
  "$luajit" "$root/tools/test.lua" \
    m5_arm64_jit_fail_closed_safepoint_runtime

env LUA_PATH="$lua_path" "$luajit" "$root/tests/t-upvalue-identity.lua"
env LJ_TEST_ROOT="$root" LUA_PATH="$lua_path" "$luajit" "$root/tests/t-jit-cell-loops.lua"

LJ_CI_RESTORE_NEEDED=0
echo "arm64_jit_fail_closed_gate OK: independent scalar LOOP capabilities, integer FORL, literal-true JFUNCF, bounded first-side entry and lifecycle contracts sound"
