#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_mcode_commit_contract SKIP: requires native macOS arm64"
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
pauth_xcflags="$ordinary_xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-mcode-commit-split.c
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
    echo "ARM64 mcode-commit contract could not restore arm64 build" >&2
    saved_status=$restore_status
  fi
  exit "$saved_status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" != 1 &&
   test "${LJ_TEST_RUN_LOCK_HELD:-}" != 1; then
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 &&
       test $((lock_now-lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 mcode-commit contract lock timed out: $lock_dir" >&2
      exit 2
    fi
    sleep 0.2
  done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

for required in \
  'typedef struct LJMCodeCommitPlan {' \
  'uint64_t generation;' \
  'uint64_t mcreserve_generation;' \
  'int lj_mcode_commit_prepare(jit_State *J, MCode *top,' \
  'void lj_mcode_commit_publish(jit_State *J, const LJMCodeCommitPlan *plan)' \
  'mcode_reservation_begin(J);' \
  'J->mcreserve_generation != generation' \
  'mcode_set_current_mode(J, MCPROT_RUN);' \
  'J->mctop = plan->newtop;' \
  'J->mcreserve_generation = plan->generation+1u;' \
  'mcode_reservation_invalidate(J);' \
  '!lj_mcode_commit_prepare(J, top, &plan)' \
  'lj_mcode_commit_publish(J, &plan);'; do
  grep -F "$required" "$root/src/lj_mcode.c" "$root/src/lj_mcode.h" \
    "$root/src/lj_jit.h" \
    >/dev/null || {
    echo "ARM64 split mcode-commit invariant changed: $required" >&2
    exit 1
  }
done
for forbidden in 'lj_mem_' 'lj_gc2_smr_' 'lj_trace_err' 'lj_vmevent'; do
  publish_body=$(sed -n \
    '/^void lj_mcode_commit_publish/,/^}/p' "$root/src/lj_mcode.c")
  if printf '%s\n' "$publish_body" | grep -F "$forbidden" >/dev/null; then
    echo "mcode commit publication gained forbidden operation: $forbidden" >&2
    exit 1
  fi
done

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-mcode-commit.XXXXXX")
fixture=$tmpdir/t-arm64-jit-mcode-commit-split
fixture_obj=$tmpdir/t-arm64-jit-mcode-commit-split.o
pauth_fixture=$tmpdir/t-arm64-jit-mcode-commit-split-arm64e
pauth_obj=$tmpdir/t-arm64-jit-mcode-commit-split-arm64e.o
restore_needed=1

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
test "$(lipo -archs "$archive")" = arm64
for symbol in _lj_mcode_commit_prepare _lj_mcode_commit_publish; do
  nm "$archive" | grep -F " T $symbol" >/dev/null || {
    echo "ordinary ARM64 archive lost $symbol" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # flags intentionally expand to compiler arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $ordinary_xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
run=1
while test "$run" -le "${LJ_ARM64_MCODE_COMMIT_RUNS:-2}"; do
  "$fixture"
  run=$((run+1))
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
test "$(lipo -archs "$archive")" = arm64e

# shellcheck disable=SC2086 # flags intentionally expand to compiler arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
run=1
while test "$run" -le "${LJ_ARM64_MCODE_COMMIT_PAUTH_RUNS:-2}"; do
  "$pauth_fixture"
  run=$((run+1))
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
restore_needed=0

echo "arm64_jit_mcode_commit_contract OK: generation-bound protection preparation rejected stale identical plans and exact mctop publication ran on ARM64/ARM64e"
