#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_first_side_production_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_first_side_production_contract
lj_ci_tmpdir lj-arm64-first-side
tmpdir=$LJ_CI_TMPDIR
base_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL'
helper_xcflags="$base_xcflags -DLJ_TRACE_TEST_HELPERS"
pauth_helper_xcflags="$helper_xcflags -DLUAJIT_ENABLE_CET_BR"
fixture_source=$root/tests/t-arm64-jit-first-side-production.c
trace_source=$root/src/lj_trace.c
test -f "$fixture_source"
lj_ci_set_restore "${LJ_TEST_RESTORE_TARGET_FLAGS:--arch arm64}" \
  "${LJ_TEST_RESTORE_XCFLAGS:-$helper_xcflags}"

check_macros() {
  target=$1 flags=$2 tag=$3
  for feature in GDBJIT PERFTOOLS; do
    macros=$tmpdir/macros-$tag-$feature.txt
    set -f
    # shellcheck disable=SC2086 # compiler and target/profile flags are word lists.
    set -- $LJ_CI_CC $LJ_CI_CFLAGS $LJ_CI_CPPFLAGS $target \
      $LJ_CI_TARGET_CFLAGS -mmacosx-version-min="$LJ_CI_MINVER" \
      $flags -DLUAJIT_USE_$feature -I"$root/src" -dM -E -x c \
      -include lj_arch.h /dev/null
    if "$@" >"$macros"; then :; else
      status=$?; set +f; return "$status"
    fi
    set +f
    expected=0
    if test "$feature" = PERFTOOLS; then expected=1; fi
    grep -E "^#define LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+$expected\$" \
      "$macros" >/dev/null || {
      echo "first-side gate mismatch with $feature ($tag)" >&2
      return 1
    }
    grep -E '^#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
      "$macros" >/dev/null || {
      echo "broad ARM64 side gate opened with $feature ($tag)" >&2
      return 1
    }
  done
}

check_profile_macros() {
  target=$1 flags=$2 arch=$3 pauth=$4 bti=$5 tag=$6
  macros=$tmpdir/macros-$tag.txt
  set -f
  # shellcheck disable=SC2086 # compiler and target/profile flags are word lists.
  set -- $LJ_CI_CC $LJ_CI_CFLAGS $LJ_CI_CPPFLAGS $target \
    $LJ_CI_TARGET_CFLAGS -mmacosx-version-min="$LJ_CI_MINVER" \
    $flags -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null
  if "$@" >"$macros"; then :; else
    status=$?; set +f; return "$status"
  fi
  set +f
  for setting in \
    "LJ_TARGET_ARM64 1" "LJ_ABI_PAUTH $pauth" \
    "LJ_ABI_BRANCH_TRACK $bti" \
    'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
    'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 0' \
    'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
    'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0'; do
    grep -E "^#define $setting\$" "$macros" >/dev/null || {
      echo "first-side profile mismatch ($tag): $setting" >&2
      return 1
    }
  done
  if grep -E '^#define LJ_ARM64_(FIRST_SIDE_PUBLISH|SIDE_ASM)_TEST' \
      "$macros" >/dev/null; then
    echo "production profile enabled an ARM64 side test seam ($tag)" >&2
    return 1
  fi
}

check_production_routing() {
  region=$tmpdir/first-side-routing.c
  awk '/^static int trace_stop_arm64_first_side/ { copy=1 }
       copy { print }
       copy && /^}/ { exit }' "$trace_source" >"$region"
  # Routing is checked at operation boundaries rather than against a fixed
  # child IR shape, trace name, exit number, or bytecode offset.
  for required in LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED \
      trace_arm64_side_parent_capture trace_arm64_side_parent_snapshot \
      trace_arm64_side_publish \
      lj_trace_arm64_side_parent_clear; do
    grep -F "$required" "$trace_source" >/dev/null || {
      echo "generic first-side transaction lost operation: $required" >&2
      exit 1
    }
  done
  # The generic semantic and post-RA admissions run in the assembler path,
  # after the selected parent snapshot has been captured and before code
  # publication. Keep this pipeline check separate from trace metadata routing.
  asm_source=$root/src/lj_asm.c
  semantic=$(grep -nF 'lj_asm_arm64_side_scalar_ir_admit(' "$asm_source" |
    sed -n '1p' | cut -d: -f1)
  postra=$(grep -nF 'lj_asm_arm64_side_postra_admit(' "$asm_source" |
    sed -n '1p' | cut -d: -f1)
  test -n "$semantic" && test -n "$postra" && test "$semantic" -lt "$postra"
  grep -F 'trace_stop_arm64_first_side(J)' "$trace_source" >/dev/null
  grep -F 'trace_arm64_side_certificate_valid_held' "$trace_source" >/dev/null
  grep -F 'trace_arm64_first_side_retire_edge' "$trace_source" >/dev/null
  # No fixture may arm the synthetic one-shot publisher in this production
  # contract; all positives below come from ordinary hotexit traffic.
  if grep -E 'lj_trace_test_arm64_first_side_publish_(arm|read)\(' \
      "$fixture_source" >/dev/null; then
    echo "production fixture calls the one-shot publication seam" >&2
    exit 1
  fi
}

build_and_run() {
  arch=$1 target=$2 flags=$3 tag=$4 pauth=$5 bti=$6
  lj_ci_build_profile "$arch" "$target" "$flags" "$tag"
  check_profile_macros "$target" "$flags" "$arch" "$pauth" "$bti" "$tag"
  if test "$tag" = smoke; then
    if nm "$LJ_CI_ARCHIVE" | grep -F ' T _lj_trace_test_reset_exit_stats' >/dev/null; then
      echo "ordinary no-helper archive unexpectedly exports trace helpers" >&2
      exit 1
    fi
  else
    lj_ci_require_symbols lj_trace_test_reset_exit_stats \
      lj_trace_test_admission_side_clean_releases
    if nm "$LJ_CI_ARCHIVE" | grep -E \
        '_lj_trace_test_arm64_first_side_publish_(arm|read)$' >/dev/null; then
      echo "production archive retained a one-shot publication seam ($tag)" >&2
      exit 1
    fi
  fi
  fixture=$tmpdir/first-side-$tag
  lj_ci_compile_fixture "$fixture" "$fixture_source"
  if test "$arch" = arm64e; then
    otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
  else
    otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+ALL' >/dev/null
  fi
  if test "$tag" = smoke; then
    lj_ci_run_fixture 180 "$fixture"
  else
    for run in $(seq 1 "$runs"); do
      lj_ci_run_fixture 180 "$fixture" gc-claim
      lj_ci_run_fixture 180 "$fixture" scoped
      lj_ci_run_fixture 180 "$fixture" full-flush
    done
  fi
}

runs=${LJ_ARM64_FIRST_SIDE_PRODUCTION_RUNS:-2}
pauth_runs=${LJ_ARM64_FIRST_SIDE_PRODUCTION_PAUTH_RUNS:-2}
case $runs:$pauth_runs in
  *[!0-9:]*|0:*|*:0) echo "invalid first-side repetition count" >&2; exit 2 ;;
esac
check_production_routing
check_macros '-arch arm64' "$base_xcflags" arm64
check_macros '-arch arm64e -mbranch-protection=bti' \
  "$base_xcflags -DLUAJIT_ENABLE_CET_BR" arm64e

# The no-helper run proves ordinary production registration. Helper builds add
# independent certificate mutation, native entry, edge identity, GC-claim,
# scoped-retirement and full-flush checks from the same compiled fixture.
build_and_run arm64 '-arch arm64' "$base_xcflags" smoke 0 0
runs=$runs
build_and_run arm64 '-arch arm64' "$helper_xcflags" arm64 0 0
runs=$pauth_runs
build_and_run arm64e '-arch arm64e -mbranch-protection=bti' \
  "$pauth_helper_xcflags" arm64e 1 1

lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo "arm64_jit_first_side_production_contract OK: generic first-child certificate, native ARM64/arm64e publication, entry and retirement"
