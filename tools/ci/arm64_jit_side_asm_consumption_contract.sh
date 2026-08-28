#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_side_asm_consumption_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
ordinary_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
probe_xcflags="$ordinary_xcflags -DLJ_ARM64_SIDE_ASM_TEST"
pauth_probe_xcflags="$probe_xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-side-asm-consumption.c
lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=

cleanup() {
  saved_status=$?
  restore_status=0
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
        XCFLAGS="$ordinary_xcflags" >/dev/null 2>&1 || restore_status=$?
    if test "$restore_status" = 0; then
      env MACOSX_DEPLOYMENT_TARGET="$minver" \
        make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
          XCFLAGS="$ordinary_xcflags" >/dev/null 2>&1 || restore_status=$?
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
    echo "ARM64 side-assembler contract could not restore arm64 build" >&2
    saved_status=$restore_status
  fi
  exit "$saved_status"
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
       test $((lock_now-lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 side-assembler contract lock timed out: $lock_dir" >&2
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
}

acquire_lock
test -f "$fixture_source" || {
  echo "missing ARM64 side-assembler fixture: $fixture_source" >&2
  exit 1
}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-side-asm.XXXXXX")
fixture=$tmpdir/t-arm64-jit-side-asm-consumption
fixture_obj=$tmpdir/t-arm64-jit-side-asm-consumption.o
macros=$tmpdir/macros-arm64.txt
pauth_fixture=$tmpdir/t-arm64-jit-side-asm-consumption-arm64e
pauth_obj=$tmpdir/t-arm64-jit-side-asm-consumption-arm64e.o
pauth_macros=$tmpdir/macros-arm64e.txt
bad_macros=$tmpdir/bad-macros.txt
restore_needed=1

# The bypass is deliberately impossible in an ordinary helper build, and the
# special build is valid only while production side recording remains closed.
if "$cc" -arch arm64 -mmacosx-version-min="$minver" \
     -DLUAJIT_MT_ARM64_BOOTSTRAP \
     -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL \
     -DLJ_ARM64_SIDE_ASM_TEST -I"$root/src" \
     -E -x c -include lj_arch.h /dev/null >"$bad_macros" 2>&1; then
  echo "ARM64 side-assembler probe compiled without trace test helpers" >&2
  exit 1
fi
grep -F 'LJ_ARM64_SIDE_ASM_TEST requires LJ_TRACE_TEST_HELPERS' \
  "$bad_macros" >/dev/null

for required in \
  'defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_ARM64_SIDE_ASM_TEST)' \
  'LJ_ARM64_SIDE_ASM_TEST requires closed ARM64 side recording' \
  'asm_test_side_probe_capture(J);' \
  'asm_test_side_probe_parentmap(as->parentmap, as->parentmap_n);' \
  'asm_test_side_probe_note(LJ_ARM64_SIDE_ASM_PROBE_PREHEAD);' \
  'asm_test_side_probe_postra(as->mcp,' \
  'asm_test_side_probe_tail(certified_parent_mcode, tail_pc);' \
  'asm_test_side_probe_note(LJ_ARM64_SIDE_ASM_PROBE_FINAL);' \
  'asm_test_side_probe_note(LJ_ARM64_SIDE_ASM_PROBE_MARKER);' \
  '(void)asm_test_side_probe_finish(T);' \
  'lj_trace_err(J, LJ_TRERR_NYIIR);'; do
  grep -F "$required" "$root/src/lj_asm.c" "$root/src/lj_arch.h" \
    >/dev/null || {
    echo "ARM64 side-assembler probe lost containment/proof: $required" >&2
    exit 1
  }
done
grep -F 'defined(LJ_ARM64_SIDE_ASM_TEST)' "$root/src/lj_trace.c" >/dev/null
grep -F 'lj_asm_arm64_test_side_probe_ingress(parent, exitno)' \
  "$root/src/lj_trace.c" >/dev/null
for required in \
  'static int trace_arm64_side_asm_test_preflight(jit_State *J,' \
  'lj_asm_arm64_test_side_probe_active(J->parent, J->exitno)' \
  'lj_trace_arm64_first_side_loop_valid(' \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER' \
  'if (J->parent != 0) {' \
  'trace_arm64_side_asm_test_preflight(J, pc, &L)'; do
  grep -F "$required" "$root/src/lj_trace.c" >/dev/null || {
    echo "ARM64 side-assembler recorder preflight changed: $required" >&2
    exit 1
  }
done

for required in \
  "jit.opt.start('hotloop=1','hotexit=1','maxtrace=3')" \
  'if bias~=0 then i=i+1 end' \
  'lj_trace_test_reset_exit_stats();' \
  'lj_trace_test_reset_exittab_stats();' \
  'lj_asm_arm64_test_side_probe_arm(PROBE_PARENT, PROBE_EXIT);' \
  'lj_asm_arm64_test_force_exitstub_mcode_retry(1);' \
  'assert(probe.stages == LJ_ARM64_SIDE_ASM_PROBE_ALL);' \
  'assert(probe.capture_count == 2);' \
  'assert(probe.parent == PROBE_PARENT);' \
  'assert(probe.child == PROBE_CHILD);' \
  'assert(probe.exitno == PROBE_EXIT);' \
  'assert(probe.cert_body == root);' \
  'assert(probe.cert_mcode == root_mcode);' \
  'assert(probe.cert_continuation == continuation);' \
  'assert(probe.parentmap0 == REGSP(RID_X28, SPS_NONE));' \
  'assert(probe.entry[0] == A64I_LE(A64I_BTI_J));' \
  'A64F_D(RID_X27) | A64F_M(RID_X28)' \
  'assert(probe.tail_target == root_mcode);' \
  'assert(lj_asm_arm64_b26_encode(' \
  'assert(probe.tail_ins == expected_tail);' \
  'assert(probe.marker == TRACE_ARM64_INT_SIDE_ADMITTED);' \
  'assert(lj_trace_test_mcode_retries() == 1);' \
  'assert(lj_trace_test_abort_count() == 2);' \
  'assert(lj_trace_test_exittab_allocs() == 2);' \
  'assert(lj_trace_test_exittab_frees() == 2);' \
  'assert(slot2 == NULL);' \
  'assert(J->curfinal == NULL);' \
  'assert(L->cframe == saved_cframe);' \
  'assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);' \
  'assert(lj_tg_load_jit_base(tg) == NULL);' \
  'assert(side_parent_cert_zero(&J->arm64_side_parent));' \
  'assert(gc2_smr_readers_acq(g) == 0);' \
  'assert(trace_nchild_acq(root) == root_nchild && root_nchild == 0);' \
  'assert(trace_nextside_acq(root) == root_nextside && root_nextside == 0);' \
  'assert(snap_count_acq(&root_snap[PROBE_EXIT]) != SNAPCOUNT_DONE);' \
  'assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_fallback);' \
  'assert(call_probe(L, 3, 0) == 3);'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 side-assembler fixture lost required proof: $required" >&2
    exit 1
  }
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$probe_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$probe_xcflags"

# shellcheck disable=SC2086 # probe_xcflags intentionally expands.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $probe_xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 0' \
  'LJ_ABI_BRANCH_TRACK 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_SIDE_ASM_TEST 1'; do
  grep -E "^#define ${setting}$" "$macros" >/dev/null || {
    echo "ARM64 side-assembler probe gate mismatch: $setting" >&2
    exit 1
  }
done
test "$(lipo -archs "$archive")" = arm64
for symbol in \
  _lj_asm_arm64_test_side_probe_arm \
  _lj_asm_arm64_test_side_probe_ingress \
  _lj_asm_arm64_test_side_probe_active \
  _lj_asm_arm64_test_side_probe_read; do
  nm "$archive" | grep -F " T $symbol" >/dev/null || {
    echo "ARM64 side-assembler probe archive lost $symbol" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # probe_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $probe_xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+ALL' >/dev/null

ordinary_runs=${LJ_ARM64_SIDE_ASM_RUNS:-2}
run=1
while test "$run" -le "$ordinary_runs"; do
  "$fixture"
  run=$((run+1))
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_probe_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_probe_xcflags"

# shellcheck disable=SC2086 # pauth_probe_xcflags intentionally expands.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_probe_xcflags -I"$root/src" \
  -dM -E -x c -include lj_arch.h /dev/null >"$pauth_macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_SIDE_ASM_TEST 1'; do
  grep -E "^#define ${setting}$" "$pauth_macros" >/dev/null || {
    echo "ARM64e side-assembler probe gate mismatch: $setting" >&2
    exit 1
  }
done
test "$(lipo -archs "$archive")" = arm64e

# shellcheck disable=SC2086 # pauth_probe_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_probe_xcflags -I"$root/src" \
  -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null

pauth_runs=${LJ_ARM64_SIDE_ASM_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  "$pauth_fixture"
  run=$((run+1))
done

# Leave the shared checkout in the ordinary experimental configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
if nm "$archive" | grep -E \
     '_lj_asm_arm64_test_side_probe_(arm|ingress|active|read)$' \
     >/dev/null; then
  echo "ordinary ARM64 helper build retained special side-probe APIs" >&2
  exit 1
fi
restore_needed=0

echo "arm64_jit_side_asm_consumption_contract OK: exact first-side assembly and MCODELM recapture proved on ARM64/ARM64e without publication"
