#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_lua_leaf_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi
lj_ci_profile_init "$root" arm64_jit_lua_leaf_contract
lj_ci_tmpdir lj-arm64-lua-leaf
tmpdir=$LJ_CI_TMPDIR
flags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS'
export LUA_PATH="$root/tests/lib/?.lua;$root/src/?.lua;$root/src/?/init.lua;;"
for profile in arm64-assert arm64e-assert arm64-release arm64e-release; do
  case "$profile" in
    arm64-assert) arch=arm64; target='-arch arm64'; extra=-DLUA_USE_ASSERT ;;
    arm64e-assert) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; extra='-DLUA_USE_ASSERT -DLUAJIT_ENABLE_CET_BR' ;;
    arm64-release) arch=arm64; target='-arch arm64'; extra= ;;
    arm64e-release) arch=arm64e; target='-arch arm64e -mbranch-protection=bti'; extra=-DLUAJIT_ENABLE_CET_BR ;;
  esac
  lj_ci_build_profile "$arch" "$target" "$flags $extra" "$profile"
  lj_ci_require_symbols lj_trace_arm64_lua_leaf lj_trace_test_root_entry_successes
  fixture=$tmpdir/lua-leaf-$profile
  lj_ci_compile_fixture "$fixture" "$root/tests/t-arm64-jit-lua-leaf-calls.c"
  lj_ci_run_fixture 60 "$fixture"
  lj_ci_run_lua 60 "$root/tests/t-arm64-jit-lua-leaf-calls.lua"
  nested_fixture=$tmpdir/lua-nested-$profile
  lj_ci_compile_fixture "$nested_fixture" "$root/tests/t-arm64-jit-lua-nested-calls.c"
  lj_ci_run_fixture 60 "$nested_fixture"
  lj_ci_run_lua 60 "$root/tests/t-arm64-jit-lua-nested-calls.lua"
  varargs_fixture=$tmpdir/lua-varargs-$profile
  lj_ci_compile_fixture "$varargs_fixture" "$root/tests/t-arm64-jit-lua-varargs.c"
  lj_ci_run_fixture 90 "$varargs_fixture"
  lj_ci_run_lua 90 "$root/tests/t-arm64-jit-lua-varargs.lua"
  child_fixture=$tmpdir/lua-child-$profile
  lj_ci_compile_fixture "$child_fixture" "$root/tests/t-arm64-jit-lua-child-exits.c"
  lj_ci_run_fixture 60 "$child_fixture"
  lj_ci_run_lua 60 "$root/tests/t-arm64-jit-lua-child-exits.lua"
  compound_fixture=$tmpdir/lua-compound-$profile
  lj_ci_compile_fixture "$compound_fixture" "$root/tests/t-arm64-jit-lua-compound-exits.c"
  lj_ci_run_fixture 60 "$compound_fixture"
  lj_ci_run_lua 60 "$root/tests/t-arm64-jit-lua-compound-exits.lua"
  arguments_fixture=$tmpdir/lua-arguments-$profile
  lj_ci_compile_fixture "$arguments_fixture" "$root/tests/t-arm64-jit-lua-arguments.c"
  lj_ci_run_fixture 60 "$arguments_fixture"
  lj_ci_run_lua 60 "$root/tests/t-arm64-jit-lua-arguments.lua"
  sparse_fixture=$tmpdir/lua-sparse-$profile
  lj_ci_compile_fixture "$sparse_fixture" "$root/tests/t-arm64-jit-lua-sparse-exits.c"
  lj_ci_run_fixture 60 "$sparse_fixture"
  lj_ci_run_lua 60 "$root/tests/t-arm64-jit-lua-sparse-exits.lua"
  normalized_fixture=$tmpdir/lua-normalized-$profile
  lj_ci_compile_fixture "$normalized_fixture" "$root/tests/t-arm64-jit-lua-normalized-args.c"
  lj_ci_run_fixture 60 "$normalized_fixture"
  lj_ci_run_lua 60 "$root/tests/t-arm64-jit-lua-normalized-args.lua"
  return_fixture=$tmpdir/lua-return-$profile
  lj_ci_compile_fixture "$return_fixture" "$root/tests/t-arm64-jit-lua-return-exits.c"
  lj_ci_run_fixture 60 "$return_fixture"
  lj_ci_run_lua 60 "$root/tests/t-arm64-jit-lua-return-exits.lua"
done
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo 'arm64_jit_lua_leaf_contract OK: four profiles; native, nested fixed calls, fixed CALL/VARG leaves, identity/single-operation/compound/sparse child exits, normalized fixed arguments, primitive literals, fixed two-result returns, terminal returns and local/parameter assignments, independent admission and ordinary publication/refusal'
