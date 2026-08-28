#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
source_file=$root/src/lj_gc.c
header_file=$root/src/lj_gc.h
tg_header=$root/src/lj_tg.h
fixture_source=$root/tests/t-arm64-jit-sealed-root-publish.c

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_sealed_root_publish_contract SKIP: requires native macOS arm64"
  exit 0
fi

for required in \
  'lj_gc_linkobj_new_sealed_prepare(global_State *g, GCobj *o,' \
  'void lj_gc_linkobj_new_sealed_publish(LJGCNewRootPublishPlan *plan)' \
  'void lj_gc_linkobj_new_sealed_abort(LJGCNewRootPublishPlan *plan)' \
  'LJ_TG_ROOT_PENDING_SEALED_TRACE' \
  'lj_tg_gcroot_pending_owner_try(' \
  'lj_tg_gcroot_pending_owner_release('; do
  grep -F "$required" "$source_file" "$header_file" "$tg_header" \
    >/dev/null || {
      echo "sealed root publication contract missing: $required" >&2
      exit 1
    }
done

grep -F 'uint32_t gcroot_pending_owner;' "$tg_header" >/dev/null
grep -F 'offsetof(TGState, gcroot_pending_owner)' "$tg_header" >/dev/null
grep -F 'LJ_TG_ROOT_PENDING_FLUSH' "$source_file" >/dev/null
grep -F 'lj_tg_gcroot_pending_xchg_acqrel(tg, NULL)' "$source_file" \
  >/dev/null

publish_text=$(sed -n \
  '/^void lj_gc_linkobj_new_sealed_publish(/,/^}/p' "$source_file")
prepare_text=$(sed -n \
  '/^int lj_gc_linkobj_new_sealed_prepare(/,/^}/p' "$source_file")
flush_text=$(sed -n \
  '/^static uint32_t gc_flush_root_pending_tg(/,/^}/p' "$source_file")
test -n "$publish_text"
test -n "$prepare_text"
test -n "$flush_text"
if printf '%s\n' "$publish_text" | \
   grep -E '^[[:space:]]*(for|while)[[:space:]]*\(|^[[:space:]]*do[[:space:]]*\{' \
   >/dev/null; then
  echo "sealed root publish gained a source retry loop" >&2
  exit 1
fi
for forbidden in \
  'lj_mem_' 'malloc(' 'calloc(' 'realloc(' 'free(' \
  'lj_gc2_smr' 'lj_thr_retry' 'lj_err_' 'lj_gc_step' 'lj_vmevent'; do
  if printf '%s\n' "$publish_text" | grep -F "$forbidden" >/dev/null; then
    echo "sealed root publish gained forbidden work: $forbidden" >&2
    exit 1
  fi
done
link_line=$(printf '%s\n' "$publish_text" | \
  grep -nF 'gc_root_set_next_rel(o, plan->pending_head)' | cut -d: -f1)
expect_line=$(printf '%s\n' "$publish_text" | \
  grep -nF 'expect = plan->pending_head' | cut -d: -f1)
cas_line=$(printf '%s\n' "$publish_text" | \
  grep -nF 'lj_tg_gcroot_pending_cas(tg, &expect, o)' | cut -d: -f1)
commit_line=$(printf '%s\n' "$publish_text" | \
  grep -nF 'lj_arena_root_construct_commit(rootstate.a, rootstate.cell)' | \
  cut -d: -f1)
release_line=$(printf '%s\n' "$publish_text" | \
  grep -nF 'lj_tg_gcroot_pending_owner_release(' | tail -1 | cut -d: -f1)
test -n "$link_line" && test -n "$expect_line" && test -n "$cas_line" && \
  test -n "$commit_line" && test -n "$release_line"
test "$link_line" -lt "$expect_line" && test "$expect_line" -lt "$cas_line" && \
  test "$cas_line" -lt "$commit_line" && test "$commit_line" -lt "$release_line"
test "$(printf '%s\n' "$publish_text" | \
  grep -Fc 'gc_root_set_next_rel(o, plan->pending_head)')" = 1
test "$(printf '%s\n' "$publish_text" | \
  grep -Fc 'expect = plan->pending_head')" = 1
test "$(printf '%s\n' "$publish_text" | \
  grep -Fc 'lj_tg_gcroot_pending_cas(tg, &expect, o)')" = 1
test "$(printf '%s\n' "$publish_text" | \
  grep -Fc 'lj_arena_root_construct_commit(rootstate.a, rootstate.cell)')" = 1
test "$(printf '%s\n' "$publish_text" | \
  grep -Fc 'lj_tg_gcroot_pending_owner_release(')" = 1
if printf '%s\n' "$publish_text" | \
   grep -F 'gc_root_construct_commit(&rootstate)' >/dev/null; then
  echo "sealed root publish regained generic/huge constructor completion" >&2
  exit 1
fi

prepare_clear_line=$(printf '%s\n' "$prepare_text" | \
  grep -nF 'gc_new_root_publish_plan_clear(plan)' | cut -d: -f1)
prepare_try_line=$(printf '%s\n' "$prepare_text" | \
  grep -nF 'lj_tg_gcroot_pending_owner_try(' | cut -d: -f1)
prepare_header_line=$(printf '%s\n' "$prepare_text" | \
  grep -nF 'gc_publishobj_header_at(g, o, o)' | cut -d: -f1)
prepare_claim_line=$(printf '%s\n' "$prepare_text" | \
  grep -nF 'gc_root_construct_claimed_at(g, o, o, &rootstate)' | \
  cut -d: -f1)
prepare_head_line=$(printf '%s\n' "$prepare_text" | \
  grep -nF 'plan->pending_head = lj_tg_gcroot_pending_acq(tg)' | \
  cut -d: -f1)
prepare_arena_line=$(printf '%s\n' "$prepare_text" | \
  grep -nF 'plan->arena = rootstate.a' | cut -d: -f1)
prepare_cell_line=$(printf '%s\n' "$prepare_text" | \
  grep -nF 'plan->cell = rootstate.cell' | cut -d: -f1)
prepare_armed_line=$(printf '%s\n' "$prepare_text" | \
  grep -nF 'plan->armed = LJ_GC_NEW_ROOT_PLAN_ARMED' | cut -d: -f1)
test -n "$prepare_clear_line" && test -n "$prepare_try_line" && \
  test -n "$prepare_header_line" && test -n "$prepare_claim_line" && \
  test -n "$prepare_head_line" && test -n "$prepare_arena_line" && \
  test -n "$prepare_cell_line" && test -n "$prepare_armed_line"
test "$prepare_clear_line" -lt "$prepare_try_line" && \
  test "$prepare_try_line" -lt "$prepare_header_line" && \
  test "$prepare_header_line" -le "$prepare_claim_line" && \
  test "$prepare_claim_line" -lt "$prepare_head_line" && \
  test "$prepare_head_line" -lt "$prepare_arena_line" && \
  test "$prepare_arena_line" -lt "$prepare_cell_line" && \
  test "$prepare_cell_line" -lt "$prepare_armed_line"
for singleton in \
  'gc_new_root_publish_plan_clear(plan)' \
  'lj_tg_gcroot_pending_owner_try(' \
  'gc_publishobj_header_at(g, o, o)' \
  'gc_root_construct_claimed_at(g, o, o, &rootstate)' \
  'plan->pending_head = lj_tg_gcroot_pending_acq(tg)' \
  'plan->arena = rootstate.a' \
  'plan->cell = rootstate.cell' \
  'plan->armed = LJ_GC_NEW_ROOT_PLAN_ARMED'; do
  test "$(printf '%s\n' "$prepare_text" | grep -Fc "$singleton")" = 1
done

flush_try_line=$(printf '%s\n' "$flush_text" | \
  grep -nF 'lj_tg_gcroot_pending_owner_try(' | cut -d: -f1)
flush_main_line=$(printf '%s\n' "$flush_text" | \
  grep -nF 'head = lj_tg_gcroot_pending_xchg_acqrel(tg, NULL)' | \
  cut -d: -f1)
flush_after_line=$(printf '%s\n' "$flush_text" | \
  grep -nF 'after_main = lj_tg_gcroot_pending_after_main_xchg_acqrel(tg, NULL)' | \
  cut -d: -f1)
flush_release_line=$(printf '%s\n' "$flush_text" | \
  grep -nF 'lj_tg_gcroot_pending_owner_release(' | cut -d: -f1)
test -n "$flush_try_line" && test -n "$flush_main_line" && \
  test -n "$flush_after_line" && test -n "$flush_release_line"
test "$flush_try_line" -lt "$flush_main_line" && \
  test "$flush_main_line" -lt "$flush_after_line" && \
  test "$flush_after_line" -lt "$flush_release_line"
test "$(printf '%s\n' "$flush_text" | \
  grep -Fc 'lj_tg_gcroot_pending_owner_try(')" = 1
test "$(printf '%s\n' "$flush_text" | \
  grep -Fc 'head = lj_tg_gcroot_pending_xchg_acqrel(tg, NULL)')" = 1
test "$(printf '%s\n' "$flush_text" | \
  grep -Fc 'after_main = lj_tg_gcroot_pending_after_main_xchg_acqrel(tg, NULL)')" = 1
test "$(printf '%s\n' "$flush_text" | \
  grep -Fc 'lj_tg_gcroot_pending_owner_release(')" = 1

for required in \
  'pthread_create(&thread, NULL, flush_attempt_main, &attempt)' \
  'attempt.flushed == 0' \
  'oldhead == obj2gco(seed)' \
  'old_after_head == obj2gco(after_seed)' \
  'lj_gcroot_pending_hint_rel(g, 0)' \
  'lj_gcroot_pending_hint_acq(g) != 0' \
  'LJ_TG_ROOT_PENDING_SEALED_TRACE' \
  'lj_gc_linkobj_new_sealed_prepare' \
  'lj_gc_linkobj_new_sealed_publish' \
  'lj_gc_linkobj_new_sealed_abort' \
  '!lj_gc_linkobj_new_sealed_prepare(g, obj2gco(huge), &plan)' \
  'lj_gc_linkobj_new(g, obj2gco(republished)) == LJ_GC_ROOT_LINKED' \
  'lj_mem_freegco_unpublished(g, abandoned, (GCSize)sizeof(GCupval))'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "sealed root fixture lost coverage: $required" >&2
    exit 1
  }
done

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
ordinary_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
pauth_xcflags="$ordinary_xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
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
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  if test -n "$tmpdir"; then
    rm -rf "$tmpdir"
  fi
  if test "$saved_status" = 0 && test "$restore_status" != 0; then
    echo "sealed root contract could not restore ordinary ARM64 build" >&2
    saved_status=$restore_status
  fi
  exit "$saved_status"
}

if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" != 1 &&
   test "${LJ_TEST_RUN_LOCK_HELD:-}" != 1; then
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  lock_announced=0
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now-lock_started)) -ge "$lock_timeout"; then
      echo "sealed root contract lock timed out: $lock_dir" >&2
      exit 2
    fi
    if test "$lock_announced" = 0; then
      echo "waiting for Lua test runner lock: $lock_dir" >&2
      lock_announced=1
    fi
    sleep 0.2
  done
  lock_held=1
fi
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-sealed-root.XXXXXX")
restore_needed=1
trap cleanup EXIT HUP INT TERM
if test "$lock_held" = 1; then
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

build_and_run() {
  target_flags=$1
  xcflags=$2
  tag=$3
  expected_arch=$4
  fixture_obj=$tmpdir/fixture-$tag.o
  fixture=$tmpdir/fixture-$tag

  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" clean TARGET_FLAGS="$target_flags" \
      XCFLAGS="$xcflags"
  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" -j"$jobs" TARGET_FLAGS="$target_flags" \
      XCFLAGS="$xcflags"
  test "$(lipo -archs "$archive")" = "$expected_arch" || {
    echo "sealed root archive architecture mismatch: $target_flags" >&2
    exit 1
  }
  # shellcheck disable=SC2086 # flags intentionally expand to compiler args.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
    -c "$fixture_source" -o "$fixture_obj"
  # shellcheck disable=SC2086 # target flags intentionally expand.
  "$cc" $target_flags -mmacosx-version-min="$minver" \
    "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
  "$fixture"
  "$fixture"
}

build_and_run '-arch arm64' "$ordinary_xcflags" arm64 arm64
build_and_run '-arch arm64e -mbranch-protection=bti' "$pauth_xcflags" \
  arm64e arm64e

echo "arm64_jit_sealed_root_publish_contract OK: bounded root publication and peer-flush exclusion ran on ARM64/ARM64e; ordinary ARM64 will be restored"
