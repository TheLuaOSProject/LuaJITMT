#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_first_side_publish_contract SKIP: requires native macOS arm64"
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
publish_xcflags="$ordinary_xcflags -DLJ_ARM64_FIRST_SIDE_PUBLISH_TEST"
pauth_publish_xcflags="$publish_xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-first-side-publish.c
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
    echo "first-side publication contract could not restore ARM64 build" >&2
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
      echo "first-side publication contract lock timed out: $lock_dir" >&2
      if test -f "$lock_dir/owner"; then
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
  echo "missing first-side publication fixture: $fixture_source" >&2
  exit 1
}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-first-side-publish.XXXXXX")
missing_helpers=$tmpdir/missing-helpers.txt
mutual_exclusion=$tmpdir/mutual-exclusion.txt
debug_registration=$tmpdir/debug-registration.txt
cell_resume_region=$tmpdir/arm64-cell-resume.txt
restore_needed=1

# The one-shot seam is impossible without helpers, cannot coexist with the
# assembler-abort seam, and excludes callback-based debug/perf registration.
if "$cc" -arch arm64 -mmacosx-version-min="$minver" \
     -DLUAJIT_MT_ARM64_BOOTSTRAP \
     -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL \
     -DLJ_ARM64_FIRST_SIDE_PUBLISH_TEST -I"$root/src" \
     -E -x c -include lj_arch.h /dev/null >"$missing_helpers" 2>&1; then
  echo "first-side publication seam compiled without trace helpers" >&2
  exit 1
fi
grep -F 'LJ_ARM64_FIRST_SIDE_PUBLISH_TEST requires LJ_TRACE_TEST_HELPERS' \
  "$missing_helpers" >/dev/null

if "$cc" -arch arm64 -mmacosx-version-min="$minver" \
     -DLUAJIT_MT_ARM64_BOOTSTRAP \
     -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS \
     -DLJ_ARM64_FIRST_SIDE_PUBLISH_TEST -DLJ_ARM64_SIDE_ASM_TEST \
     -I"$root/src" -E -x c -include lj_arch.h /dev/null \
     >"$mutual_exclusion" 2>&1; then
  echo "first-side publication and assembler seams compiled together" >&2
  exit 1
fi
grep -F 'ARM64 side assembler and first-side publish tests are mutually exclusive' \
  "$mutual_exclusion" >/dev/null

if "$cc" -arch arm64 -mmacosx-version-min="$minver" \
     -DLUAJIT_MT_ARM64_BOOTSTRAP \
     -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS \
     -DLJ_ARM64_FIRST_SIDE_PUBLISH_TEST -DLUAJIT_USE_PERFTOOLS \
     -I"$root/src" -E -x c -include lj_arch.h /dev/null \
     >"$debug_registration" 2>&1; then
  echo "first-side publication seam compiled with perf registration" >&2
  exit 1
fi
grep -F 'LJ_ARM64_FIRST_SIDE_PUBLISH_TEST excludes deferred debug/perf registration' \
  "$debug_registration" >/dev/null

# Keep source checks deliberately narrow: runtime assertions below own the
# transaction proof, while these checks preserve only the isolated test seam.
for required in \
  'LJ_ARM64_FIRST_SIDE_PUBLISH_TEST requires closed ARM64 side recording' \
  'LJ_TRACE_ARM64_FIRST_SIDE_PUBLISH_DONE' \
  'lj_trace_test_arm64_first_side_publish_arm(' \
  'lj_trace_test_arm64_first_side_publish_read(' \
  'if (first >= LJ_TRACE_ARM64_FIRST_SIDE_PUBLISH_ARMED) {' \
  'if (first == LJ_TRACE_ARM64_FIRST_SIDE_PUBLISH_DONE)'; do
  grep -F "$required" "$root/src/lj_arch.h" "$root/src/lj_trace.h" \
    "$root/src/lj_trace.c" >/dev/null || {
    echo "first-side publication seam lost required source: $required" >&2
    exit 1
  }
done

# vm_exit_interp must resume the continuation CGET through this TG's recording
# overlay. A global/static dispatch here skips CGET recording, so the child
# loads its temporary destination from stale stack memory on native entry.
sed -n \
  '/|6:  \/\/ Dispatch a local cell opcode without function-header reshaping\./,/|  br_auth RB/p' \
  "$root/src/vm_arm64.dasc" >"$cell_resume_region"
test -s "$cell_resume_region" || {
  echo "missing ARM64 local-cell resume dispatch region" >&2
  exit 1
}
for required in \
  '|  add TMP0, DISPATCH, INS, uxtb #3' \
  '|  ldr RB, [TMP0]'; do
  grep -F "$required" "$cell_resume_region" >/dev/null || {
    echo "ARM64 local-cell resume lost TG dispatch: $required" >&2
    exit 1
  }
done
if grep -F 'GG_G2DISP' "$cell_resume_region" >/dev/null; then
  echo "ARM64 local-cell resume regained global/static dispatch" >&2
  exit 1
fi

for required in \
  "jit.opt.start('hotloop=1','hotexit=1','maxtrace=3')" \
  'assert(traceref_safe(J, PROBE_CHILD) == NULL);' \
  'lj_trace_test_arm64_first_side_publish_arm(' \
  'assert(probe.state == LJ_TRACE_ARM64_FIRST_SIDE_PUBLISH_DONE);' \
  'assert(probe.attempts == 1);' \
  'assert(probe.publishes == 1);' \
  'assert(probe.failure == 0);' \
  'assert(trace_nchild_acq(root) == 1);' \
  'assert(trace_nextside_acq(root) == PROBE_CHILD);' \
  'assert(snap_count_acq(&root_snap[PROBE_EXIT]) == SNAPCOUNT_DONE);' \
  'assert(first_exit == PROBE_CHILD_EXIT);' \
  'trace_exittarget_arm64_encode(g, child_mcode)' \
  'LJ_ARENA_LIFETIME_LIVE' \
  'LJ_ARENA_ROOT_MEMBER' \
  'assert(actual == expected);' \
  'assert(mcode[0] == A64I_LE(A64I_BTI_J));' \
  'assert(calls == 1);' \
  'assert(first_parent == PROBE_CHILD);' \
  'assert(traceref_safe(J, PROBE_GRANDCHILD) == NULL);' \
  'assert(side_parent_cert_zero(&J->arm64_side_parent));' \
  'assert(gc2_smr_readers_acq(g) == 0);'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "first-side publication fixture lost required proof: $required" >&2
    exit 1
  }
done

build_and_run() {
  target_flags=$1
  xcflags=$2
  tag=$3
  expected_arch=$4
  expected_pauth=$5
  expected_bti=$6
  runs=$7
  fixture_obj=$tmpdir/fixture-$tag.o
  fixture=$tmpdir/fixture-$tag
  macros=$tmpdir/macros-$tag.txt

  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" clean TARGET_FLAGS="$target_flags" \
      XCFLAGS="$xcflags"
  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" -j"$jobs" TARGET_FLAGS="$target_flags" \
      XCFLAGS="$xcflags"
  test "$(lipo -archs "$archive")" = "$expected_arch" || {
    echo "first-side publication archive architecture mismatch: $tag" >&2
    exit 1
  }

  # shellcheck disable=SC2086 # target/feature flags intentionally expand.
  "$cc" $target_flags -mmacosx-version-min="$minver" $xcflags \
    -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
  for setting in \
    'LJ_TARGET_ARM64 1' \
    "LJ_ABI_PAUTH $expected_pauth" \
    "LJ_ABI_BRANCH_TRACK $expected_bti" \
    'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
    'LJ_ARM64_FIRST_SIDE_PUBLISH_TEST 1'; do
    grep -E "^#define ${setting}$" "$macros" >/dev/null || {
      echo "first-side publication gate mismatch ($tag): $setting" >&2
      exit 1
    }
  done

  for symbol in \
    _lj_trace_test_arm64_first_side_publish_arm \
    _lj_trace_test_arm64_first_side_publish_read; do
    nm "$archive" | grep -F " T $symbol" >/dev/null || {
      echo "first-side publication archive lost $symbol ($tag)" >&2
      exit 1
    }
  done

  # shellcheck disable=SC2086 # target/feature flags intentionally expand.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
    -c "$fixture_source" -o "$fixture_obj"
  # shellcheck disable=SC2086 # target flags intentionally expand.
  "$cc" $target_flags -mmacosx-version-min="$minver" \
    "$fixture_obj" "$archive" -lm -pthread -o "$fixture"

  run=1
  while test "$run" -le "$runs"; do
    "$fixture"
    run=$((run+1))
  done
}

build_and_run '-arch arm64' "$publish_xcflags" arm64 arm64 0 0 \
  "${LJ_ARM64_FIRST_SIDE_PUBLISH_RUNS:-2}"
build_and_run '-arch arm64e -mbranch-protection=bti' \
  "$pauth_publish_xcflags" arm64e arm64e 1 1 \
  "${LJ_ARM64_FIRST_SIDE_PUBLISH_PAUTH_RUNS:-2}"

# Leave the shared checkout in the ordinary experimental helper configuration
# and prove the one-shot seam exports no symbols there.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
if nm "$archive" | grep -E \
     '_lj_trace_test_arm64_first_side_publish_(arm|read)$' >/dev/null; then
  echo "ordinary ARM64 helper build retained first-side publication APIs" >&2
  exit 1
fi
restore_needed=0

echo "arm64_jit_first_side_publish_contract OK: exact first child published and executed twice on ARM64/ARM64e while the ordinary side gate stayed closed"
