#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
source_file=$root/src/lj_gdbjit.c
header_file=$root/src/lj_gdbjit.h
trace_source=$root/src/lj_trace.c
fixture_source=$root/tests/t-arm64-jit-gdbjit-prepare.c

for required in \
  'GDBJITPrepared *lj_gdbjit_preparetrace' \
  'int lj_gdbjit_committrace' \
  'void lj_gdbjit_aborttrace' \
  'lj_mem_new_nothrow' \
  'gdbjit_buildobj_bounded(&ctx, filenamelen)' \
  'filenamelen*2u+alignslop' \
  'relevant->symfile_size != 0' \
  'lj_gdbjit_test_descriptor_lock_acquire' \
  'lj_gdbjit_test_prepared_object_size' \
  'lj_gdbjit_aborttrace(J2G(J), prep)'; do
  grep -F "$required" "$source_file" >/dev/null || {
    echo "GDBJIT prepare source lost contract: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'DSTR(ctx->filename)' "$source_file")" = 2
test "$(grep -Fc 'SECTALIGN(ctx->p' "$source_file")" = 1

for required in \
  'typedef struct GDBJITPrepared GDBJITPrepared;' \
  'lj_gdbjit_preparetrace(jit_State *J, GCtrace *T,' \
  'lj_gdbjit_committrace(GCtrace *T, GDBJITPrepared *prep)' \
  'lj_gdbjit_aborttrace(global_State *g, GDBJITPrepared *prep)' \
  'lj_gdbjit_test_descriptor_lock_acquire(void)' \
  'lj_gdbjit_test_prepared_symfile_size(' \
  'The caller must retain T and serialize its retirement through commit.'; do
  grep -F "$required" "$header_file" >/dev/null || {
    echo "GDBJIT prepare header lost contract: $required" >&2
    exit 1
  }
done

commit_text=$(sed -n '/^int lj_gdbjit_committrace(/,/^}/p' "$source_file")
abort_text=$(sed -n '/^void lj_gdbjit_aborttrace(/,/^}/p' "$source_file")
test -n "$commit_text" && test -n "$abort_text"
test "$(printf '%s\n' "$commit_text" | grep -Fc 'gdbjit_lock_try()')" = 1
if printf '%s\n' "$commit_text" | \
   grep -E '^[[:space:]]*(for|while)[[:space:]]*\(|^[[:space:]]*do[[:space:]]*\{' \
   >/dev/null; then
  echo "GDBJIT commit gained a source loop" >&2
  exit 1
fi
for forbidden in \
  'lj_mem_' 'malloc(' 'calloc(' 'realloc(' 'free(' \
  'lj_gc2_smr' 'gdbjit_lock_acquire' 'lj_thr_retry' 'lj_err_' \
  'lj_gc_' 'lj_vmevent' 'lua_'; do
  if printf '%s\n' "$commit_text" | grep -F "$forbidden" >/dev/null; then
    echo "GDBJIT commit gained forbidden work: $forbidden" >&2
    exit 1
  fi
done

attempt_line=$(printf '%s\n' "$commit_text" | \
  grep -nF 'eo->state = GDBJIT_COMMIT_ATTEMPTED;' | cut -d: -f1)
lock_line=$(printf '%s\n' "$commit_text" | \
  grep -nF 'gdbjit_lock_try()' | cut -d: -f1)
committed_line=$(printf '%s\n' "$commit_text" | \
  grep -nF 'eo->state = GDBJIT_COMMITTED;' | cut -d: -f1)
trace_line=$(printf '%s\n' "$commit_text" | \
  grep -nF 'trace_gdbjit_entry_rel(T, (void *)eo);' | cut -d: -f1)
descriptor_line=$(printf '%s\n' "$commit_text" | \
  grep -nF 'gdbjit_desc_first_rel(&eo->entry);' | cut -d: -f1)
relevant_line=$(printf '%s\n' "$commit_text" | \
  grep -nF 'gdbjit_desc_relevant_rel(&eo->entry);' | cut -d: -f1)
action_line=$(printf '%s\n' "$commit_text" | \
  grep -nF 'gdbjit_desc_action_rel(GDBJIT_REGISTER);' | cut -d: -f1)
callback_line=$(printf '%s\n' "$commit_text" | \
  grep -nF '__jit_debug_register_code();' | cut -d: -f1)
unlock_line=$(printf '%s\n' "$commit_text" | \
  grep -nF 'gdbjit_lock_release();' | tail -n 1 | cut -d: -f1)
test -n "$attempt_line" && test -n "$lock_line" && \
test -n "$committed_line" && test -n "$trace_line" && \
test -n "$descriptor_line" && test -n "$relevant_line" && \
test -n "$action_line" && test -n "$callback_line" && \
test -n "$unlock_line" && \
test "$attempt_line" -lt "$lock_line" && \
test "$lock_line" -lt "$committed_line" && \
test "$committed_line" -lt "$trace_line" && \
test "$trace_line" -lt "$descriptor_line" && \
test "$descriptor_line" -lt "$relevant_line" && \
test "$relevant_line" -lt "$action_line" && \
test "$action_line" -lt "$callback_line" && \
test "$callback_line" -lt "$unlock_line"

printf '%s\n' "$abort_text" | \
  grep -F 'eo->state != GDBJIT_COMMITTED' >/dev/null
printf '%s\n' "$abort_text" | grep -F 'lj_mem_free(g, eo, eo->sz)' >/dev/null
if printf '%s\n' "$abort_text" | \
   grep -E 'eo->target|trace_gdbjit_entry|gdbjit_lock' >/dev/null; then
  echo "GDBJIT abort regained target or descriptor dependence" >&2
  exit 1
fi
if grep -E 'lj_gdbjit_(prepare|commit)trace' "$trace_source" >/dev/null; then
  echo "GDBJIT split was wired into production trace publication prematurely" >&2
  exit 1
fi
if grep -F 'lj_mem_newt' "$source_file" >/dev/null; then
  echo "GDBJIT preparation retained the throwing allocator" >&2
  exit 1
fi

for required in \
  'chunkname_with_payload(1200u)' \
  'chunkname_with_payload(6000u)' \
  'lj_gdbjit_test_force_prepare_alloc_omit();' \
  'lj_gdbjit_test_descriptor_lock_acquire() == 1' \
  'stats->prepare_bounds_omits == 1' \
  'stats->prepare_alloc_omits == 1' \
  'stats->commit_lock_omits == 1' \
  'stats->register_callbacks_ready == 1' \
  'exercise_private_prepare(L, T);' \
  'exercise_filename_boundary(L, T);' \
  'lj_gdbjit_preparetrace(J, &target, parent)' \
  'lj_gdbjit_preparetrace(J, &target, &impostor)' \
  'lj_gdbjit_committrace(&other, prep)' \
  'symfile_size == object_size' \
  'hi == lo+1u' \
  'trace_gdbjit_entry_acq(T) != NULL'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "GDBJIT prepare fixture lost proof: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'lj_gdbjit_committrace(&target, prep)' "$fixture_source")" = 2

if test "${LJ_GDBJIT_SOURCE_ONLY:-0}" = 1; then
  echo "arm64_jit_gdbjit_prepare_contract OK: bounded source contract"
  exit 0
fi

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_gdbjit_prepare_contract SKIP: requires native macOS arm64"
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
gdb_xcflags="$ordinary_xcflags -DLUAJIT_USE_GDBJIT"
test_xcflags="$gdb_xcflags -DLJ_GDBJIT_TEST_HELPERS"
pauth_xcflags="$test_xcflags -DLUAJIT_ENABLE_CET_BR"
pauth_gdb_xcflags="$gdb_xcflags -DLUAJIT_ENABLE_CET_BR"
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
    echo "ARM64 GDBJIT prepare contract could not restore arm64 build" >&2
    saved_status=$restore_status
  fi
  exit "$saved_status"
}

trap cleanup EXIT HUP INT TERM

if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" != 1 && \
   test "${LJ_TEST_RUN_LOCK_HELD:-}" != 1; then
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  lock_announced=0
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 && \
       test $((lock_now-lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 GDBJIT prepare contract lock timed out: $lock_dir" >&2
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
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-gdbjit-prepare.XXXXXX")
fixture=$tmpdir/t-arm64-jit-gdbjit-prepare
fixture_obj=$tmpdir/t-arm64-jit-gdbjit-prepare.o
source_obj=$tmpdir/lj_gdbjit-arm64.o
test_source_obj=$tmpdir/lj_gdbjit-arm64-test.o
pauth_fixture=$tmpdir/t-arm64-jit-gdbjit-prepare-arm64e
pauth_fixture_obj=$tmpdir/t-arm64-jit-gdbjit-prepare-arm64e.o
pauth_source_obj=$tmpdir/lj_gdbjit-arm64e.o
pauth_test_source_obj=$tmpdir/lj_gdbjit-arm64e-test.o
macros=$tmpdir/macros-arm64.txt
pauth_macros=$tmpdir/macros-arm64e.txt

restore_needed=1
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$test_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$test_xcflags"
test "$(lipo -archs "$archive")" = arm64

# shellcheck disable=SC2086 # test_xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $test_xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_HASJIT 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$macros" >/dev/null || {
    echo "ARM64 GDBJIT prepare gate mismatch: $setting" >&2
    exit 1
  }
done
nm "$archive" | grep ' T _lj_gdbjit_preparetrace$' >/dev/null
nm "$archive" | grep ' T _lj_gdbjit_test_reset$' >/dev/null

# Compile both the production translation unit and fixture with warnings fatal.
# shellcheck disable=SC2086 # gdb_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -fomit-frame-pointer -Wall -Wextra -Werror \
  -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -U_FORTIFY_SOURCE \
  -fno-stack-protector -DLUAJIT_UNWIND_EXTERNAL -arch arm64 \
  -mmacosx-version-min="$minver" $gdb_xcflags -I"$root/src" \
  -c "$source_file" -o "$source_obj"
# shellcheck disable=SC2086 # test_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -fomit-frame-pointer -Wall -Wextra -Werror \
  -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -U_FORTIFY_SOURCE \
  -fno-stack-protector -DLUAJIT_UNWIND_EXTERNAL -arch arm64 \
  -mmacosx-version-min="$minver" $test_xcflags -I"$root/src" \
  -c "$source_file" -o "$test_source_obj"
# shellcheck disable=SC2086 # test_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $test_xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
run=1
while test "$run" -le "${LJ_ARM64_GDBJIT_RUNS:-2}"; do
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

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -dM -E -x c -include lj_arch.h /dev/null >"$pauth_macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_HASJIT 1' \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$pauth_macros" >/dev/null || {
    echo "ARM64e GDBJIT prepare gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_gdb_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -fomit-frame-pointer -Wall -Wextra -Werror \
  -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -U_FORTIFY_SOURCE \
  -fno-stack-protector -DLUAJIT_UNWIND_EXTERNAL -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_gdb_xcflags -I"$root/src" -c "$source_file" \
  -o "$pauth_source_obj"
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -fomit-frame-pointer -Wall -Wextra -Werror \
  -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -U_FORTIFY_SOURCE \
  -fno-stack-protector -DLUAJIT_UNWIND_EXTERNAL -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" -c "$source_file" \
  -o "$pauth_test_source_obj"
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" -c "$fixture_source" \
  -o "$pauth_fixture_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_fixture_obj" "$archive" \
  -lm -pthread -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
run=1
while test "$run" -le "${LJ_ARM64_GDBJIT_PAUTH_RUNS:-2}"; do
  "$pauth_fixture"
  run=$((run+1))
done

# Leave the checkout in the ordinary thin experimental ARM64 configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
test "$(lipo -archs "$archive")" = arm64
if nm "$archive" | grep -E \
     ' T _lj_gdbjit_(preparetrace|test_reset)$' >/dev/null; then
  echo "ordinary ARM64 archive retained GDBJIT prepare/test helpers" >&2
  exit 1
fi
restore_needed=0

echo "arm64_jit_gdbjit_prepare_contract OK: bounded registration and one-shot omissions ran on ARM64/ARM64e; ordinary ARM64 was restored"
