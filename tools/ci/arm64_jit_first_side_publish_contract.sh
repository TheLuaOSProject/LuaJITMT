#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"

if lj_ci_native_arm64_or_skip arm64_jit_first_side_publish_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_first_side_publish_contract
lj_ci_tmpdir lj-arm64-first-side-publish
tmpdir=$LJ_CI_TMPDIR
ordinary_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
publish_xcflags="$ordinary_xcflags -DLJ_ARM64_FIRST_SIDE_PUBLISH_TEST"
pauth_publish_xcflags="$publish_xcflags -DLUAJIT_ENABLE_CET_BR"
fixture_source=$root/tests/t-arm64-jit-first-side-publish.c
trace_source=$root/src/lj_trace.c
archive=$LJ_CI_ARCHIVE
lj_ci_set_restore "${LJ_TEST_RESTORE_TARGET_FLAGS:--arch arm64}" \
  "${LJ_TEST_RESTORE_XCFLAGS:-$ordinary_xcflags}"
test -f "$fixture_source" || {
  echo "missing first-side publication fixture: $fixture_source" >&2
  exit 1
}

require_order() {
  region=$1
  before=$2
  after=$3
  label=$4
  before_line=$(awk -v needle="$before" \
    'index($0, needle) { print NR; exit }' "$region")
  after_line=$(awk -v needle="$after" \
    'index($0, needle) { print NR; exit }' "$region")
  if test -z "$before_line" || test -z "$after_line" || \
     test "$before_line" -ge "$after_line"; then
    echo "first-side retirement ordering changed: $label" >&2
    exit 1
  fi
}

missing_helpers=$tmpdir/missing-helpers.txt
mutual_exclusion=$tmpdir/mutual-exclusion.txt
debug_registration=$tmpdir/debug-registration.txt
cell_resume_region=$tmpdir/arm64-cell-resume.txt
publish_child_region=$tmpdir/first-side-publish-child.txt
retire_plan_region=$tmpdir/first-side-retire-plan.txt
retire_edge_region=$tmpdir/first-side-retire-edge.txt
retire_topology_region=$tmpdir/first-side-retire-topology.txt
retire_unlink_region=$tmpdir/first-side-retire-unlink.txt
retire_disconnect_region=$tmpdir/first-side-retire-disconnect.txt
retire_gc_claim_region=$tmpdir/first-side-retire-gc-claim.txt
retire_scope_region=$tmpdir/first-side-retire-scope.txt
retire_flushall_prepass_region=$tmpdir/first-side-retire-flushall-prepass.txt
retire_flushall_region=$tmpdir/first-side-retire-flushall.txt
retire_fixture_region=$tmpdir/first-side-retire-fixture.txt
fallback_fixture_region=$tmpdir/first-side-fallback-fixture.txt
full_flush_fixture_region=$tmpdir/first-side-full-flush-fixture.txt
scoped_fixture_region=$tmpdir/first-side-scoped-fixture.txt
mode_fixture_region=$tmpdir/first-side-mode-fixture.txt
# The one-shot seam is impossible without helpers, cannot coexist with the
# assembler-abort seam, and excludes callback-based debug/perf registration.
set -f
# shellcheck disable=SC2086 # compiler and user flags are intentional lists.
if $LJ_CI_CC $LJ_CI_CFLAGS $LJ_CI_CPPFLAGS -arch arm64 \
     $LJ_CI_TARGET_CFLAGS -mmacosx-version-min="$LJ_CI_MINVER" \
     -DLUAJIT_MT_ARM64_BOOTSTRAP \
     -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL \
     -DLJ_ARM64_FIRST_SIDE_PUBLISH_TEST -I"$root/src" \
     -E -x c -include lj_arch.h /dev/null >"$missing_helpers" 2>&1; then
  echo "first-side publication seam compiled without trace helpers" >&2
  exit 1
fi
grep -F 'LJ_ARM64_FIRST_SIDE_PUBLISH_TEST requires LJ_TRACE_TEST_HELPERS' \
  "$missing_helpers" >/dev/null

if $LJ_CI_CC $LJ_CI_CFLAGS $LJ_CI_CPPFLAGS -arch arm64 \
     $LJ_CI_TARGET_CFLAGS -mmacosx-version-min="$LJ_CI_MINVER" \
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

if $LJ_CI_CC $LJ_CI_CFLAGS $LJ_CI_CPPFLAGS -arch arm64 \
     $LJ_CI_TARGET_CFLAGS -mmacosx-version-min="$LJ_CI_MINVER" \
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
set +f

# Keep the original publication-seam checks deliberately narrow. Runtime
# assertions below own its transaction proof; retirement has separate bounded
# helper and call-site ordering checks.
for required in \
  'LJ_ARM64_FIRST_SIDE_PUBLISH_TEST requires closed ARM64 side recording' \
  'LJ_ARM64_FIRST_SIDE_PUBLISH_TEST requires closed production first-side recording' \
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

# The compiled publication fixture now mutates every sealed certificate field
# and executes prepublish, published-entry, and retirement checks. Keep the
# runtime proof authoritative instead of pinning this path to a source-level
# descriptor/template spelling.

# The exact inverse is finite: compare/exchange the authenticated raw parent
# edge to fallback, publish retirement at each caller, then compare/exchange
# the sole first-child topology. The terminal snapshot is intentionally sticky.
awk '/^static int trace_arm64_first_side_retire_plan\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$retire_plan_region"
awk '/^static int trace_arm64_first_side_retire_edge\(/ && \
       !/;[[:space:]]*$/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$retire_edge_region"
awk '/^static int trace_arm64_first_side_retire_topology\(/ && \
       !/;[[:space:]]*$/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$retire_topology_region"
awk '/^static void trace_unlink_side_chain\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$retire_unlink_region"
awk '/^static void trace_retire_disconnect\(/ && !/;[[:space:]]*$/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$retire_disconnect_region"
awk '/^int LJ_FASTCALL lj_trace_retire_gc_claim\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$retire_gc_claim_region"
awk '/^static void trace_scope_clear_slot\(/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$retire_scope_region"
awk '/^static void trace_flushall_arm64_first_side_prepass\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' \
  "$trace_source" >"$retire_flushall_prepass_region"
awk '/^static int trace_flushall_direct\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$retire_flushall_region"
for region in "$retire_plan_region" "$retire_edge_region" \
  "$retire_topology_region" \
  "$retire_unlink_region" "$retire_disconnect_region" \
  "$retire_gc_claim_region" "$retire_scope_region" \
  "$retire_flushall_prepass_region" "$retire_flushall_region"; do
  test -s "$region" || {
    echo "first-side retirement source region is missing: $region" >&2
    exit 1
  }
done

# Runtime fixture modes below exercise the selected exit bounds, child/fallback
# raw target identity, and inverse topology on both ARM64 ABIs.
grep -F 'trace_exittarget_arm64_raw_cas_acqrel(' \
  "$retire_edge_region" >/dev/null || {
  echo "first-side raw retirement helper lost authenticated edge CAS" >&2
  exit 1
}
for required in \
  'if (plan.raw_target_bits == plan.fallback_encoding_bits)' \
  'if (plan.raw_target_bits != plan.child_encoding_bits)' \
  'ARM64 first-side edge has wrong authenticated target' \
  'trace_arm64_first_side_pointer_bits(' \
  'plan.fallback_encoding_bits)'; do
  grep -F "$required" "$retire_edge_region" >/dev/null || {
    echo "first-side raw retirement rejection lost: $required" >&2
    exit 1
  }
done
for required in \
  'if (plan.raw_target_bits != plan.fallback_encoding_bits)' \
  'retired ARM64 first-side edge is not exact fallback'; do
  grep -F "$required" "$retire_topology_region" >/dev/null || {
    echo "first-side retired fallback validation lost: $required" >&2
    exit 1
  }
done
require_order "$retire_topology_region" 'trace_nextside_cas_acqrel(' \
  'trace_nchild_cas_acqrel(' 'nextside removal before child-count removal'
grep -F 'trace_arm64_first_side_retire_topology(' \
  "$retire_unlink_region" >/dev/null || {
  echo "side-chain unlink lost exact first-side topology inverse" >&2
  exit 1
}
require_order "$retire_unlink_region" \
  'trace_arm64_first_side_retire_topology(' 'trace_nextside_rel(' \
  'exact topology inverse before legacy side-chain mutation'
require_order "$retire_unlink_region" \
  'trace_arm64_first_side_retire_topology(' \
  'root = traceref_safe(J, rootno);' \
  'idempotent exact inverse before former-parent lookup'
for required in \
  'else if (trace_arm64_first_side_retire_candidate(T)) {' \
  'trace_unlink_side_chain(J, T, traceno, rootno);' \
  'trace_exittab_reset(J, T);'; do
  grep -F "$required" "$retire_disconnect_region" >/dev/null || {
    echo "retirement disconnect lost exact first-side branch: $required" >&2
    exit 1
  }
done
require_order "$retire_disconnect_region" \
  'trace_unlink_side_chain(J, T, traceno, rootno);' \
  'trace_exittab_reset(J, T);' \
  'exact topology inverse before child exit reset'
for forbidden in trace_exittarget_reset_one trace_nextside_rel \
  trace_nchild_dec_acqrel; do
  if grep -F "$forbidden" "$retire_edge_region" \
       "$retire_topology_region" >/dev/null; then
    echo "first-side retirement helper regained legacy mutation: $forbidden" >&2
    exit 1
  fi
done
if grep -E 'snap_(topslot|count)_(rel|cas_acqrel|xchg_acqrel)' \
     "$retire_plan_region" "$retire_edge_region" \
     "$retire_topology_region" >/dev/null; then
  echo "first-side retirement helper rewrites the sticky parent snapshot" >&2
  exit 1
fi

require_order "$retire_gc_claim_region" \
  'trace_arm64_first_side_retire_edge(' 'trace_retire_at_epoch(' \
  'GC claim edge inverse before retirement publication'
require_order "$retire_gc_claim_region" 'trace_retire_at_epoch(' \
  'trace_retire_disconnect(' \
  'GC claim retirement publication before generic disconnect'
require_order "$retire_scope_region" \
  'trace_arm64_first_side_retire_edge(' 'trace_retire_at_epoch(' \
  'scoped edge inverse before retirement publication'
require_order "$retire_scope_region" 'trace_retire_at_epoch(' \
  'trace_retire_disconnect(' \
  'scoped retirement publication before generic disconnect'
require_order "$retire_flushall_prepass_region" \
  'trace_arm64_first_side_retire_edge(' 'trace_retire(' \
  'full-flush edge inverse before retirement publication'
require_order "$retire_flushall_prepass_region" 'trace_retire(' \
  'trace_retire_disconnect(' \
  'full-flush retirement publication before generic disconnect'
if grep -E 'trace_(exittab_reset|flushroot|unpatch|slot_retire|retired_slot_release)|traceslot_|proto_trace_rel' \
     "$retire_flushall_prepass_region" >/dev/null; then
  echo "first-side full-flush prepass performs destructive slot/root teardown" >&2
  exit 1
fi
require_order "$retire_flushall_region" \
  'trace_flushall_arm64_first_side_prepass(' \
  'for (i = (ptrdiff_t)sizetrace-1; i > 0; i--)' \
  'first-side full-flush prepass before destructive loop'
for required in \
  'sizetrace = trace_sizetrace_acq(J);' \
  'if (trace_arm64_first_side_retire_candidate(T) &&' \
  'la_load64_acq(&T->retire_epoch) != 0' \
  'trace_retire(J2G(J), T);' \
  'trace_retired_link_listed_acq(T)'; do
  grep -F "$required" "$retire_flushall_region" >/dev/null || {
    echo "full-flush retirement guard changed: $required" >&2
    exit 1
  }
done

# The publication fixture's runtime assertions cover native child entry,
# authenticated edge identity, GC claim, full/scoped flush, and fallback
# reentry. Compile and execute those proofs below instead of grepping the C
# fixture for a fixed prototype, snapshot layout, or assertion spelling.

build_and_run() {
  target_flags=$1
  xcflags=$2
  tag=$3
  expected_arch=$4
  expected_pauth=$5
  expected_bti=$6
  runs=$7
  fixture=$tmpdir/fixture-$tag
  macros=$tmpdir/macros-$tag.txt

  lj_ci_build_profile "$expected_arch" "$target_flags" "$xcflags" "$tag"
  test "$(lipo -archs "$archive")" = "$expected_arch" || {
    echo "first-side publication archive architecture mismatch: $tag" >&2
    exit 1
  }

  set -f
  # shellcheck disable=SC2086 # compiler and target/profile flags are lists.
  $LJ_CI_CC $LJ_CI_CFLAGS $LJ_CI_CPPFLAGS $target_flags \
    $LJ_CI_TARGET_CFLAGS -mmacosx-version-min="$LJ_CI_MINVER" $xcflags \
    -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
  set +f
  for setting in \
    'LJ_TARGET_ARM64 1' \
    "LJ_ABI_PAUTH $expected_pauth" \
    "LJ_ABI_BRANCH_TRACK $expected_bti" \
    'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
    'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 1' \
    'LJ_ARM64_FIRST_SIDE_PUBLISH_TEST 1'; do
    grep -E "^#define ${setting}$" "$macros" >/dev/null || {
      echo "first-side publication gate mismatch ($tag): $setting" >&2
      exit 1
    }
  done

  for symbol in \
    _lj_trace_retire_gc_claim \
    _lj_trace_test_arm64_first_side_publish_arm \
    _lj_trace_test_arm64_first_side_publish_read; do
    nm "$archive" | grep -F " T $symbol" >/dev/null || {
      echo "first-side publication archive lost $symbol ($tag)" >&2
      exit 1
    }
  done

  lj_ci_compile_fixture "$fixture" "$fixture_source"

  run=1
  while test "$run" -le "$runs"; do
    lj_ci_run_fixture 180 "$fixture" gc-claim
    lj_ci_run_fixture 180 "$fixture" full-flush
    lj_ci_run_fixture 180 "$fixture" scoped
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
lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
if nm "$archive" | grep -E \
     '_lj_trace_test_arm64_first_side_publish_(arm|read)$' >/dev/null; then
  echo "ordinary ARM64 helper build retained first-side publication APIs" >&2
  exit 1
fi
echo "arm64_jit_first_side_publish_contract OK: exact first child published and retired through GC claim, full flush and scoped flush on ARM64/ARM64e with authenticated fallback and sticky publication metadata"
