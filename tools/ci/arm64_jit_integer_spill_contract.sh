#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_integer_spill_contract SKIP: requires native macOS arm64"
  exit 0
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-integer-spills.c
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
       test $((lock_now-lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 integer-spill contract lock timed out: $lock_dir" >&2
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
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-integer-spill.XXXXXX")
fixture=$tmpdir/t-arm64-jit-integer-spills
fixture_obj=$tmpdir/t-arm64-jit-integer-spills.o
pauth_fixture=$tmpdir/t-arm64-jit-integer-spills-arm64e
pauth_obj=$tmpdir/t-arm64-jit-integer-spills-arm64e.o
macros=$tmpdir/macros.txt
pauth_macros=$tmpdir/macros-arm64e.txt

# Freeze the three distinct pressure surfaces. The generated Lua body contains
# no calls, allocations, FFI, sides, stitches or function-entry traces.
for required in \
  'FIXED_N = 23' \
  'MIN_DYNAMIC_N = 23' \
  'HEAVY_N = 26' \
  '"dynamic-23-plus-k", SPILL_MIN_DYNAMIC' \
  '"heavy-26", SPILL_HEAVY' \
  'UINT64_C(0x8e987d05ec986c8b)' \
  'UINT64_C(0x9daea8d07564bb57)' \
  'UINT64_C(0x0e434c1ea83db142)' \
  'UINT64_C(0xa5e15ce0c3358d84)' \
  'UINT64_C(0x3074ad891371262f)' \
  'UINT64_C(0x697804b1a9938ca1)' \
  'assert(count == 2);' \
  'assert(count == 4);' \
  'assert(spec->kind == SPILL_HEAVY && count == 36);' \
  'assert(ir[body_i+2u].s == 4);' \
  'These unequal left/right spill pairs force asm_phi_copyspill().' \
  'expect_snapshot_tuple(root, HEAVY_N+7u, 4,' \
  'expect_snapshot_tuple(root, HEAVY_N+8u, 4,' \
  'assert(trace_nsnapmap_acq(T) == root->spec->nsnapmap);' \
  'assert(ntuples == root->spec->snapshot_tuples);' \
  'assert(irhash == root->spec->ir_fingerprint);' \
  'assert(snaphash == root->spec->snapshot_fingerprint);' \
  'UINT32_C(0xd10043ff)' \
  'assert(nsub == 1);' \
  'assert(nbackedge == 1);' \
  'POSTADMISSION_PROFILE' \
  'POSTADMISSION_STOPREQ' \
  'lj_trace_test_first_exitno() == root.loopsnap' \
  'thread interrupted: VM shutdown' \
  'assert(tvisnum(a2));' \
  'assert(numV(a2) == 2147483648.0);' \
  'expect_one_exit(HEAVY_N+7u);' \
  'trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0' \
  'lj_tg_load_jit_base(root->tg) == NULL' \
  'lj_tg_in_native_acq(root->tg) == 0' \
  'lj_tg_vmstate_load_acq(root->tg) == root->idle_vmstate'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 integer-spill fixture lost proof: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'call_and_expect_native_vector(&root, 0);' \
  "$fixture_source")" = 4
test "$(grep -Fc 'call_and_expect_native_vector(&root, 1);' \
  "$fixture_source")" = 3
test "$(grep -Fc 'SpillRoot root = spill_root_new(' \
  "$fixture_source")" = 3
if grep -E 'require\(|ffi\.|LJ_ARM64_SPILL_MEASURE' \
     "$fixture_source" >/dev/null; then
  echo "ARM64 integer-spill fixture gained an unsupported/bypass surface" >&2
  exit 1
fi

# Keep the behavioral proof coupled to the bounded post-RA validator and the
# second root-entry validation after the trace lifetime lease is published.
for required in \
  '#define SPS_FIXED' \
  '#define SPS_FIRST' \
  '#define SPS_LIMIT' \
  'static int arm64_postra_spill_slot(MSize slot, MSize capacity)' \
  'capacity = SPS_FIXED + spadjust / sizeof(int32_t);' \
  'if (highest_end <= SPS_FIXED)' \
  'MSize expected = (MSize)sps_scale(sps_align(highest_end));' \
  'if (spadjust != expected || highest_end > capacity)' \
  'view->snapmap == NULL || view->proto_bc == NULL ||' \
  'view->proto_sizebc == 0 || view->root_topslot == 0 ||' \
  'view->root_topslot > UINT8_MAX || view->base_delta != 0)' \
  'nslots < 1u+LJ_FR2 || topslot != view->root_topslot ||' \
  'nslots > view->root_topslot+1u+LJ_FR2)' \
  '(uint8_t)pcbase != view->base_delta ||' \
  'rs = source.prev;' \
  'rs = ren.prev;' \
  'if (!arm64_postra_spill_slot(regsp_spill(rs), capacity))' \
  'postraview.proto_bc = proto_bc(J->pt);' \
  'postraview.proto_sizebc = J->pt->sizebc;' \
  'postraview.root_topslot = T->topslot;' \
  'postraview.base_delta = (uint8_t)(J->baseslot-2u);'; do
  case "$required" in
    '#define '*) file=$root/src/lj_target_arm64.h ;;
    *) file=$root/src/lj_asm.c ;;
  esac
  grep -F "$required" "$file" >/dev/null || {
    echo "ARM64 integer-spill admission mismatch: $required" >&2
    exit 1
  }
done
for required in \
  'v->topslot == (MSize)pt->framesize &&' \
  'GCproto *pt = gco2pt(v->startpt);' \
  'postra.proto_bc = proto_bc(pt);' \
  'postra.proto_sizebc = pt->sizebc;' \
  'postra.root_topslot = v->topslot;' \
  'postra.base_delta = 0;'; do
  grep -F "$required" "$root/src/lj_trace.c" >/dev/null || {
    echo "ARM64 integer-spill root revalidation mismatch: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'trace_root_entry_arm64_layout_valid(&view' \
  "$root/src/lj_trace.c")" = 2

restore_needed=1
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_trace_test_root_entry_pause$' >/dev/null
nm "$archive" | grep ' T _lj_trace_test_reset_exit_stats$' >/dev/null

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_HASJIT 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$macros" >/dev/null || {
    echo "ARM64 integer-spill policy mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
ordinary_runs=${LJ_ARM64_INTEGER_SPILL_RUNS:-3}
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
  grep -F "#define $setting" "$pauth_macros" >/dev/null || {
    echo "ARM64e integer-spill policy mismatch: $setting" >&2
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
pauth_runs=${LJ_ARM64_INTEGER_SPILL_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  "$pauth_fixture"
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$pauth_fixture"

# Leave the shared checkout in the ordinary experimental ARM64 configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_integer_spill_contract OK: fixed slots, canonical 16-byte frame, copy-spill overflow, XPOLL and ARM64e/far paths passed"
