#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_scalar_loop_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-scalar-loop.c
lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$xcflags" >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$xcflags" >/dev/null 2>&1 || status=1
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

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

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
      echo "ARM64 scalar-loop contract lock timed out: $lock_dir" >&2
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
  {
    printf 'time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'cmd=%s\n' "$0"
  } >"$lock_dir/owner" 2>/dev/null || true
}

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-scalar-loop.XXXXXX")
fixture=$tmpdir/t-arm64-jit-scalar-loop
fixture_obj=$tmpdir/t-arm64-jit-scalar-loop.o
pauth_fixture=$tmpdir/t-arm64-jit-scalar-loop-arm64e
pauth_obj=$tmpdir/t-arm64-jit-scalar-loop-arm64e.o
macros=$tmpdir/macros.txt
pauth_macros=$tmpdir/macros-arm64e.txt
ops_region=$tmpdir/ops.txt

awk '/^static const IROp sub_ops/ { copying = 1 }
     copying { print }
     copying && /^static void run_lua/ { exit }' "$fixture_source" >"$ops_region"
test -s "$ops_region"
for required in \
  'IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_SUBOV, IR_GT, IR_LOOP, IR_XPOLL' \
  'IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_MULOV, IR_SLOAD, IR_GT, IR_LOOP' \
  'IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_ADDOV, IR_SLOAD, IR_GE, IR_LOOP' \
  'IR_ADDOV, IR_ADDOV, IR_LE, IR_PHI, IR_PHI' \
  'IR_ADDOV, IR_SUBOV, IR_GE, IR_PHI, IR_PHI' \
  'IR_ADDOV, IR_ADDOV, IR_NE, IR_PHI, IR_PHI' \
  'IR_SLOAD, IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_EQ, IR_ADDOV' \
  'IR_SLOAD, IR_SLOAD, IR_SLOAD, IR_SLOAD, IR_ADDOV, IR_SUBOV, IR_MULOV' \
  'IR_SLOAD, IR_GT, IR_LOOP, IR_XPOLL, IR_ADDOV, IR_SUBOV, IR_MULOV, IR_LT,' \
  'IR_SLOAD, IR_SUBOV, IR_GE, IR_LOOP, IR_XPOLL, IR_SUBOV, IR_GE'; do
  grep -F "$required" "$ops_region" >/dev/null || {
    echo "ARM64 scalar fixture lost exact IR sequence: $required" >&2
    exit 1
  }
done
if grep -E 'IR_(CONV|KNUM|ADD,|SUB,|MUL,|DIV|MOD|CALL|TNEW|SNEW|CNEW|AREF|HREF|FLOAD|XLOAD)' \
     "$ops_region" >/dev/null; then
  echo "ARM64 scalar fixture gained an unsupported IR family" >&2
  exit 1
fi

for required in \
  "while i>0 do x=x+i i=i-1 end return x end" \
  "while i<=n do x=x+i i=i+1 end return x end" \
  "while i>=1 do x=x+i i=i-1 end return x end" \
  "while i~=n do i=i+1 x=x+i end return x end" \
  "if k==7 then x=x+i else x=x-i end" \
  "while i<n do i=i+1 x=(x-s)*m end return x end" \
  "while i<n do i=i+1 x=x*3 end return x end" \
  "while i>=-2147483648 do i=i-1 end return i end" \
  'lua_Integer overflow_args[2] = { 2, 357913941 };' \
  'assert(call_scalar(L, overflow_args, 2) == 3221225469.0);' \
  'lua_Integer overflow_arg[1] = { -2147483647LL };' \
  'assert(call_scalar(L, overflow_arg, 1) == -2147483649.0);' \
  'assert(loopsnap == 5 && finalexit == 8 && overflowexit == 7);' \
  'assert(loopsnap == 6 && finalexit == 10);' \
  '4, { 20, 10, 1, 2 }, 8388610' \
  'ir[ref].op1 >= REF_FIRST && ir[ref].op1 < ref' \
  'ir[ref].op2 >= REF_FIRST && ir[ref].op2 < ref' \
  'static const lua_Integer false_args[2] = { 1, 6 };' \
  'assert(call_scalar(L, false_args, 2) == -1);' \
  'expect_one_exit(2);' \
  'trace_nins_acq(T) == semantic_end+nphi' \
  'assert(ir[ref].o == IR_RENAME);' \
  'assert(ir[ref].r < RID_MAX_GPR);' \
  'rset_test(RSET_GPR, ir[ref].r)' \
  'if (irref_isk(mapref) || (sn & SNAP_FRAME))' \
  'rs = ir[mapref].prev;' \
  'for (renref = trace_nins_acq(T); renref-- > semantic_end; )' \
  'if (ren->op1 == mapref && ren->op2 <= snapno)' \
  'assert(!ra_hasspill(regsp_spill(rs)));' \
  'assert(regsp_reg(rs) < RID_MAX_GPR);' \
  'rset_test(RSET_GPR, regsp_reg(rs))' \
  'for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)' \
  'assert(loopsnap == 3 && overflowexit == 4);' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  'POSTADMISSION_PROFILE' \
  'POSTADMISSION_STOPREQ' \
  'lj_trace_test_first_exitno() == loopsnap' \
  'expect_one_exit(overflowexit);' \
  'trace_spadjust_acq(T) == 0' \
  'TRACE_ARM64_INT_LOOP_ADMITTED' \
  '!ra_hasspill(ir[ref].s)' \
  'trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 scalar fixture lost proof: $required" >&2
    exit 1
  }
done
if grep -E 'require\(|ffi\.|"[[:space:]]*for[[:space:]]' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 scalar fixture uses an unsupported Lua surface" >&2
  exit 1
fi

for required in \
  'case IR_SLOAD: case IR_ADDOV: case IR_SUBOV: case IR_MULOV:' \
  'case IR_LT: case IR_GE: case IR_LE: case IR_GT:' \
  'case IR_EQ: case IR_NE:' \
  'case IR_ADDOV: case IR_SUBOV: case IR_MULOV:'; do
  grep -F "$required" "$root/src/lj_asm.c" >/dev/null || {
    echo "ARM64 scalar admission mismatch: $required" >&2
    exit 1
  }
done
grep -F 'regsp_reg(rs) >= RID_MAX_GPR ||' \
  "$root/src/lj_asm.c" >/dev/null || {
  echo "ARM64 scalar post-RA snapshot register check is missing" >&2
  exit 1
}
grep -F 'ren.op2 >= nsnap || ren.r >= RID_MAX_GPR ||' \
  "$root/src/lj_asm.c" >/dev/null || {
  echo "ARM64 scalar RENAME register range check is missing" >&2
  exit 1
}

restore_needed=1
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
test "$(lipo -archs "$archive")" = arm64

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E "$root/src/lj_arch.h" >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -E "^#define ${setting}$" "$macros" >/dev/null || {
    echo "ARM64 scalar-loop gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
ordinary_runs=${LJ_ARM64_SCALAR_RUNS:-3}
run=1
while test "$run" -le "$ordinary_runs"; do
  "$fixture"
  run=$((run+1))
done

LJ_TEST_RUN_LOCK_HELD=1 "$root/tools/ci/arm64_jit_ir_admission_contract.sh"

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -dM -E -x c -include lj_arch.h /dev/null >"$pauth_macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0'; do
  grep -E "^#define ${setting}$" "$pauth_macros" >/dev/null || {
    echo "ARM64e scalar-loop gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" $pauth_xcflags \
  -I"$root/src" -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
pauth_runs=${LJ_ARM64_SCALAR_PAUTH_RUNS:-3}
run=1
while test "$run" -le "$pauth_runs"; do
  "$pauth_fixture"
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$pauth_fixture"

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_scalar_loop_contract OK: checked scalar arithmetic and all signed guards executed on ARM64/ARM64e with overflow and XPOLL exits"
