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

# Freeze the ordinary nature of this contract. Trace identities come from live
# prototype/topology state, while the only admitted parent exits are the three
# coupled production descriptors. Neither historical side seam is armed.
for required in \
  '__arm64_first_side_production_first' \
  '__arm64_first_side_production_second' \
  '__arm64_first_side_production_third' \
  '__arm64_first_side_production_fourth' \
  '__arm64_first_side_production_unsupported' \
  "jit.opt.start('hotloop=1','hotexit=1','maxtrace=16')" \
  'local function third(n, bias)' \
  '__arm64_first_side_production_third=third;' \
  'local function fourth(n, bias)' \
  '__arm64_first_side_production_fourth=fourth;' \
  'local i=0' \
  'i=(i~=0 and i or i)+1' \
  'if i==0 then i=i+1 end' \
  'i=(i~=0 and i or i)+2' \
  'i=(i~=0 and i or i)+3' \
  'call_named(L, "__arm64_first_side_production_second", 3, 0) == 3' \
  'call_named(L, "__arm64_first_side_production_second", 2, 0) == 2' \
  'call_named(L, "__arm64_first_side_production_third", 3, 0) == 3' \
  'call_named(L, "__arm64_first_side_production_third", 4, 0) == 4' \
  'call_named(L, "__arm64_first_side_production_fourth", 3, 0) == 4' \
  'call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6' \
  'call_named(L, "__arm64_first_side_production_unsupported",' \
  'call_named(L, "__arm64_first_side_production_unsupported"' \
  "assert(live==10, 'expected ten production traces, got '..live); " \
  "assert(roots==6, 'expected six roots, got '..roots); " \
  "assert(sides==4, 'expected four first sides, got '..sides)" \
  'PRODUCTION_ROOT_ATTEMPTS = 64' \
  'PRODUCTION_PAIR_COUNT = 4,' \
  '*tracenop = foundno;' \
  'ExitNo exitno;' \
  'lua_Integer root_n;' \
  'lua_Integer side_n;' \
  'MSize root_nsnap;' \
  'MSize continuation_pos;' \
  'MSize child_pcpos[PRODUCTION_CHILD_NSNAP];' \
  'lua_Integer native_n;' \
  'lua_Integer native_bias;' \
  'lua_Integer native_result;' \
  'Reg inherited_reg;' \
  'Reg sload_reg;' \
  'int32_t addend;' \
  '.exitno = 2, .root_nsnap = 8, .continuation_pos = 13,' \
  '.child_pcpos = { 13, 14, 3, 17, 7 }' \
  '.inherited_reg = RID_X28, .sload_reg = RID_X27, .addend = 1' \
  '.native_n = 3, .native_bias = 1, .native_result = 4,' \
  '.exitno = 6, .root_nsnap = 9, .continuation_pos = 10,' \
  '.child_pcpos = { 10, 11, 3, 17, 7 }' \
  '.inherited_reg = RID_X27, .sload_reg = RID_X28, .addend = 1' \
  '.side_n = 3, .side_bias = 0, .side_result = 3,' \
  '.native_n = 2, .native_bias = 0, .native_result = 2,' \
  '.name = "__arm64_first_side_production_third"' \
  '.root_n = 3, .root_bias = 0, .root_result = 3,' \
  '.side_n = 4, .side_bias = 0, .side_result = 4,' \
  '.native_n = 3, .native_bias = 0, .native_result = 3,' \
  '.exitno = 7, .root_nsnap = 11, .continuation_pos = 13,' \
  'PRODUCTION_CHILD_K_ADDEND = REF_TRUE-1u' \
  'trace_nk_acq(pair->child) == PRODUCTION_CHILD_K_ADDEND' \
  'PRODUCTION_CHILD_R_PARENT, PRODUCTION_CHILD_K_ADDEND);' \
  'ins.i == pair->addend' \
  'trace_nsnap_acq(pair->root) == pair->root_nsnap' \
  'pair->exitno < pair->root_nsnap' \
  'selected_map_has_slot(pair->root, pair->exitno, 4)' \
  'proto_bc(pair->pt)+pair->continuation_pos' \
  'pair->child_pcpos[0] == pair->continuation_pos' \
  'ins.r == pair->sload_reg && ins.s == SPS_NONE' \
  'ins.r == pair->inherited_reg && ins.s == SPS_NONE' \
  'A64F_D(pair->sload_reg)' \
  'A64F_M(pair->inherited_reg)' \
  'pair->rootno, pair->exitno);' \
  'snap_count_acq(&pair->root_snap[pair->exitno]) != SNAPCOUNT_DONE' \
  'pair->root_exittab[pair->exitno]' \
  'pair->root_snap[pair->exitno]' \
  'call_named(L, pair->name, pair->root_n, pair->root_bias)' \
  'pair->root_result' \
  'call_named(L, pair->name, pair->side_n, pair->side_bias)' \
  'pair->side_result' \
  'childno = trace_nextside_acq(pair->root);' \
  'expect_post_token_request_cleanup(L, J, g, tg, &pairs[i]);' \
  'expect_native_child(L, J, &pairs[i]);' \
  'call_named(L, pair->name, pair->native_n, pair->native_bias)' \
  'pair->native_result' \
  'expect_edge(g, &pairs[i], pairs[i].child_mcode);' \
  'for (j = 0; j < i; j++)' \
  'pairs[i].rootno != pairs[j].rootno' \
  'pairs[i].rootno != pairs[j].childno' \
  'pairs[i].childno != pairs[j].childno' \
  'pairs[i].exitno != pairs[j].exitno ||' \
  'pairs[i].addend != pairs[j].addend' \
  'pairs[1].exitno == 6 && pairs[1].addend == 1' \
  'pairs[3].exitno == 6 && pairs[3].addend == 2' \
  'pairs[0].rootno != 1 && pairs[0].childno != 2' \
  'live_trace_count(J) == 10u' \
  'assert(lj_trace_test_retire_publish_calls() == 10u);' \
  'expect_return_linked_variant_closed(L, J, g, &pairs[i]);' \
  'lj_trace_test_abort_count() == 2' \
  'count_after == count_before+2u && count_after < SNAPCOUNT_DONE' \
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
test "$(grep -Fc 'local function third(n, bias)' "$fixture_source")" = 1 || {
  echo "production probes lost their single exit-7 function" >&2
  exit 1
}
test "$(grep -Fc '__arm64_first_side_production_third=third;' \
  "$fixture_source")" = 1 || {
  echo "production probes lost their single exit-7 global" >&2
  exit 1
}
test "$(grep -Fc 'local function fourth(n, bias)' "$fixture_source")" = 1 || {
  echo "production probes lost their single exit-6 +2 function" >&2
  exit 1
}
test "$(grep -Fc '__arm64_first_side_production_fourth=fourth;' \
  "$fixture_source")" = 1 || {
  echo "production probes lost their single exit-6 +2 global" >&2
  exit 1
}
third_pair=$tmpdir/third-pair.txt
sed -n '/\.name = "__arm64_first_side_production_third"/,/^    }/p' \
  "$fixture_source" >"$third_pair"
for required in \
  '.name = "__arm64_first_side_production_third"' \
  '.root_n = 3, .root_bias = 0, .root_result = 3,' \
  '.side_n = 4, .side_bias = 0, .side_result = 4,' \
  '.native_n = 3, .native_bias = 0, .native_result = 3,' \
  '.exitno = 7, .root_nsnap = 11, .continuation_pos = 13,' \
  '.child_pcpos = { 13, 14, 3, 17, 7 }' \
  '.inherited_reg = RID_X28, .sload_reg = RID_X27, .addend = 1'; do
  grep -F "$required" "$third_pair" >/dev/null || {
    echo "production exit-7 descriptor lost proof: $required" >&2
    exit 1
  }
done
fourth_pair=$tmpdir/fourth-pair.txt
sed -n '/\.name = "__arm64_first_side_production_fourth"/,/^    }/p' \
  "$fixture_source" >"$fourth_pair"
for required in \
  '.name = "__arm64_first_side_production_fourth"' \
  '.root_n = 3, .root_bias = 0, .root_result = 4,' \
  '.side_n = 5, .side_bias = 0, .side_result = 6,' \
  '.native_n = 3, .native_bias = 0, .native_result = 4,' \
  '.exitno = 6, .root_nsnap = 9, .continuation_pos = 10,' \
  '.child_pcpos = { 10, 11, 3, 17, 7 }' \
  '.inherited_reg = RID_X27, .sload_reg = RID_X28, .addend = 2'; do
  grep -F "$required" "$fourth_pair" >/dev/null || {
    echo "production exit-6 +2 descriptor lost proof: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'local i=0' "$fixture_source")" = 6 || {
  echo "production probes lost their deterministic zero loop seed" >&2
  exit 1
}
test "$(grep -Fc '.side_n = 3, .side_bias = 0, .side_result = 3,' \
  "$fixture_source")" = 1 || {
  echo "production exit-6 positive probe lost its recording trigger" >&2
  exit 1
}
test "$(grep -Fc '.side_n = 5, .side_bias = 0, .side_result = 6,' \
  "$fixture_source")" = 1 || {
  echo "production exit-6 +2 probe lost its root-linked trigger" >&2
  exit 1
}
test "$(grep -Fc '.side_n = 7, .side_bias = 0, .side_result = 9,' \
  "$fixture_source")" = 1 || {
  echo "unsupported exit-6 +3 probe lost its recording trigger" >&2
  exit 1
}
test "$(grep -Fc \
  'call_named(L, "__arm64_first_side_production_second", 3, 0) == 3' \
  "$fixture_source")" = 2 || {
  echo "production exit-6 smoke lost its repeated recording trigger" >&2
  exit 1
}
test "$(grep -Fc \
  'call_named(L, "__arm64_first_side_production_second", 2, 0) == 2' \
  "$fixture_source")" = 1 || {
  echo "production exit-6 smoke lost its separate native input" >&2
  exit 1
}
test "$(grep -Fc \
  'call_named(L, "__arm64_first_side_production_third", 3, 0) == 3' \
  "$fixture_source")" = 2 || {
  echo "production exit-7 smoke lost its root/native input" >&2
  exit 1
}
test "$(grep -Fc \
  'call_named(L, "__arm64_first_side_production_third", 4, 0) == 4' \
  "$fixture_source")" = 2 || {
  echo "production exit-7 smoke lost its repeated recording trigger" >&2
  exit 1
}
test "$(grep -Fc \
  'call_named(L, "__arm64_first_side_production_fourth", 3, 0) == 4' \
  "$fixture_source")" = 2 || {
  echo "production exit-6 +2 smoke lost its return-linked negative controls" >&2
  exit 1
}
test "$(grep -Fc \
  'call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6' \
  "$fixture_source")" = 4 || {
  echo "production exit-6 +2 smoke lost its root-linked recording trigger" >&2
  exit 1
}
test "$(grep -Fc 'attempt < PRODUCTION_ROOT_ATTEMPTS' \
  "$fixture_source")" = 2 || {
  echo "production root recording lost its bounded root-discovery coverage" >&2
  exit 1
}
test "$(grep -Fc \
  'call_named(L, "__arm64_first_side_production_unsupported"' \
  "$fixture_source")" = 1 || {
  echo "unsupported exit-6 smoke lost its bounded root trigger" >&2
  exit 1
}
test "$(grep -Fc 'for (i = 0; i < PRODUCTION_PAIR_COUNT; i++)' \
  "$fixture_source")" = 6 || {
  echo "production first-side fixture lost four-pair loop coverage" >&2
  exit 1
}
test "$(grep -Fc 'for (j = i+1u; j < PRODUCTION_PAIR_COUNT; j++)' \
  "$fixture_source")" = 2 || {
  echo "production first-side fixture lost remaining-pair edge coverage" >&2
  exit 1
}
test "$(grep -Fc 'for (j = 0; j < i; j++)' "$fixture_source")" = 1 || {
  echo "production first-side fixture lost pair uniqueness coverage" >&2
  exit 1
}
test "$(grep -Fc \
  '.exitno = 6, .root_nsnap = 9, .continuation_pos = 10,' \
  "$fixture_source")" = 3 || {
  echo "production first-side fixture lost exit-6 +1/+2/+3 coupling" >&2
  exit 1
}
if grep -E 'ExitNo selected = UINT32_MAX|PRODUCTION_(EXIT|CONTINUATION_POS)' \
     "$fixture_source" >/dev/null; then
  echo "production first-side fixture regained arbitrary exit discovery" >&2
  exit 1
fi
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

check_registration_gates() {
  target_flags=$1
  xcflags=$2
  tag=$3
  for feature in GDBJIT PERFTOOLS; do
    macros=$tmpdir/macros-$tag-$feature.txt
    # shellcheck disable=SC2086 # target/feature flags intentionally expand.
    "$cc" $target_flags -mmacosx-version-min="$minver" $xcflags \
      -DLUAJIT_USE_$feature -I"$root/src" -dM -E -x c \
      -include lj_arch.h /dev/null >"$macros"
    expected=0
    if test "$feature" = PERFTOOLS; then expected=1; fi
    grep -E "^#define LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+$expected\$" \
      "$macros" >/dev/null || {
      echo "production first-side registration gate mismatch with $feature ($tag)" >&2
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

check_registration_gates '-arch arm64' "$base_xcflags" arm64
check_registration_gates '-arch arm64e -mbranch-protection=bti' \
  "$base_xcflags -DLUAJIT_ENABLE_CET_BR" arm64e

# A truly ordinary archive (no trace helpers at all) must publish all four
# exact first sides through normal hotexit traffic.
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
