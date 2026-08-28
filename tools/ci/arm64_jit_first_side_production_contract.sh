#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_first_side_production_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
base_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
helper_xcflags="$base_xcflags -DLJ_TRACE_TEST_HELPERS"
pauth_helper_xcflags="$helper_xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-first-side-production.c
trace_source=$root/src/lj_trace.c
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
        XCFLAGS="$helper_xcflags" >/dev/null 2>&1 || restore_status=$?
    if test "$restore_status" = 0; then
      env MACOSX_DEPLOYMENT_TARGET="$minver" \
        make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
          XCFLAGS="$helper_xcflags" >/dev/null 2>&1 || restore_status=$?
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
    echo "production first-side contract could not restore ARM64 build" >&2
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
      echo "production first-side contract lock timed out: $lock_dir" >&2
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
  echo "missing production first-side fixture: $fixture_source" >&2
  exit 1
}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-first-side-production.XXXXXX")
restore_needed=1

# Freeze the ordinary nature of this contract. It discovers trace numbers from
# live prototype/topology state and never arms either historical side seam.
for required in \
  '__arm64_first_side_production_first' \
  '__arm64_first_side_production_second' \
  '__arm64_first_side_production_unsupported' \
  '*tracenop = foundno;' \
  'childno = trace_nextside_acq(pair->root);' \
  'pairs[0].rootno != 1 && pairs[0].childno != 2' \
  'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED != 0' \
  'pointer_bits(raw) == pointer_bits(encoded)' \
  'pair->child_mcode[0] == A64I_LE(A64I_BTI_J)' \
  'function_bits(actual) == function_bits(expected)' \
  'LJ_TRACE_TEST_ADMISSION_SIDE_AFTER_TOKEN' \
  'lj_trace_test_admission_side_clean_releases() == 1' \
  'lj_trace_test_admission_side_snapshot_before() == before' \
  'lj_trace_test_last_abort_error() == LJ_TRERR_NYIIR' \
  'lj_trace_retire_gc_claim(g, pair->child)' \
  'lj_trace_flushscope(J, pair->childno)' \
  'lj_trace_flushall_gc(L)'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "production first-side fixture lost proof: $required" >&2
    exit 1
  }
done
if grep -E 'lj_trace_test_arm64_first_side_publish_(arm|read)\(' \
     "$fixture_source" >/dev/null; then
  echo "production first-side fixture called the one-shot publication seam" >&2
  exit 1
fi
for required in \
  '#elif !LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED' \
  'return trace_stop_arm64_first_side(J);' \
  'lj_trace_err(J, LJ_TRERR_RETRY);' \
  'if (++J->gc_pressure_traces >= TRACE_GC_PRESSURE_BATCH)'; do
  grep -F "$required" "$trace_source" >/dev/null || {
    echo "production first-side routing lost source: $required" >&2
    exit 1
  }
done

check_macros() {
  macros=$1
  expected_pauth=$2
  expected_bti=$3
  tag=$4
  for setting in \
    'LJ_TARGET_ARM64 1' \
    "LJ_ABI_PAUTH $expected_pauth" \
    "LJ_ABI_BRANCH_TRACK $expected_bti" \
    'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
    'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 0' \
    'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
    'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0'; do
    grep -E "^#define ${setting}$" "$macros" >/dev/null || {
      echo "production first-side gate mismatch ($tag): $setting" >&2
      exit 1
    }
  done
  if grep -E '^#define LJ_ARM64_(FIRST_SIDE_PUBLISH|SIDE_ASM)_TEST' \
       "$macros" >/dev/null; then
    echo "production first-side build enabled a test seam ($tag)" >&2
    exit 1
  fi
}

check_registration_closed() {
  target_flags=$1
  xcflags=$2
  tag=$3
  for feature in GDBJIT PERFTOOLS; do
    macros=$tmpdir/macros-$tag-$feature.txt
    # shellcheck disable=SC2086 # target/feature flags intentionally expand.
    "$cc" $target_flags -mmacosx-version-min="$minver" $xcflags \
      -DLUAJIT_USE_$feature -I"$root/src" -dM -E -x c \
      -include lj_arch.h /dev/null >"$macros"
    grep -E '^#define LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
      "$macros" >/dev/null || {
      echo "production first-side gate stayed open with $feature ($tag)" >&2
      exit 1
    }
    grep -E '^#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
      "$macros" >/dev/null || {
      echo "broad ARM64 side gate opened with $feature ($tag)" >&2
      exit 1
    }
  done
}

build_archive() {
  target_flags=$1
  xcflags=$2
  tag=$3
  expected_arch=$4
  expected_pauth=$5
  expected_bti=$6
  macros=$tmpdir/macros-$tag.txt

  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" clean TARGET_FLAGS="$target_flags" \
      XCFLAGS="$xcflags"
  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" -j"$jobs" TARGET_FLAGS="$target_flags" \
      XCFLAGS="$xcflags"
  test "$(lipo -archs "$archive")" = "$expected_arch" || {
    echo "production first-side archive architecture mismatch: $tag" >&2
    exit 1
  }

  # shellcheck disable=SC2086 # target/feature flags intentionally expand.
  "$cc" $target_flags -mmacosx-version-min="$minver" $xcflags \
    -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
  check_macros "$macros" "$expected_pauth" "$expected_bti" "$tag"
  if nm "$archive" | grep -E \
       '_lj_trace_test_arm64_first_side_publish_(arm|read)$' >/dev/null; then
    echo "production archive retained publication seam symbols: $tag" >&2
    exit 1
  fi
}

compile_fixture() {
  target_flags=$1
  xcflags=$2
  tag=$3
  fixture_obj=$tmpdir/fixture-$tag.o
  fixture=$tmpdir/fixture-$tag

  # shellcheck disable=SC2086 # target/feature flags intentionally expand.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
    -c "$fixture_source" -o "$fixture_obj"
  # shellcheck disable=SC2086 # target flags intentionally expand.
  "$cc" $target_flags -mmacosx-version-min="$minver" \
    "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
  printf '%s\n' "$fixture"
}

check_registration_closed '-arch arm64' "$base_xcflags" arm64
check_registration_closed '-arch arm64e -mbranch-protection=bti' \
  "$base_xcflags -DLUAJIT_ENABLE_CET_BR" arm64e

# A truly ordinary archive (no trace helpers at all) must publish both exact
# first sides through normal hotexit traffic.
build_archive '-arch arm64' "$base_xcflags" smoke arm64 0 0
if nm "$archive" | grep -F ' T _lj_trace_test_reset_exit_stats' >/dev/null; then
  echo "ordinary no-helper archive unexpectedly exported trace helpers" >&2
  exit 1
fi
smoke_fixture=$(compile_fixture '-arch arm64' "$base_xcflags" smoke)
otool -hv "$smoke_fixture" | grep -E 'ARM64[[:space:]]+ALL' >/dev/null
"$smoke_fixture"

run_detailed() {
  target_flags=$1
  xcflags=$2
  tag=$3
  expected_arch=$4
  expected_pauth=$5
  expected_bti=$6
  runs=$7
  build_archive "$target_flags" "$xcflags" "$tag" "$expected_arch" \
    "$expected_pauth" "$expected_bti"
  nm "$archive" | grep -F ' T _lj_trace_test_reset_exit_stats' >/dev/null || {
    echo "production helper archive lost exit observation: $tag" >&2
    exit 1
  }
  fixture=$(compile_fixture "$target_flags" "$xcflags" "$tag")
  if test "$expected_arch" = arm64e; then
    otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
  else
    otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+ALL' >/dev/null
  fi
  run=1
  while test "$run" -le "$runs"; do
    "$fixture" gc-claim
    "$fixture" scoped
    "$fixture" full-flush
    run=$((run+1))
  done
}

run_detailed '-arch arm64' "$helper_xcflags" arm64 arm64 0 0 \
  "${LJ_ARM64_FIRST_SIDE_PRODUCTION_RUNS:-2}"
run_detailed '-arch arm64e -mbranch-protection=bti' \
  "$pauth_helper_xcflags" arm64e arm64e 1 1 \
  "${LJ_ARM64_FIRST_SIDE_PRODUCTION_PAUTH_RUNS:-2}"

# Leave the shared checkout in the ordinary experimental helper configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$helper_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$helper_xcflags"
restore_needed=0

echo "arm64_jit_first_side_production_contract OK"
