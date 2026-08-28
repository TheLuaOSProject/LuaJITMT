#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
callback_source=$root/src/lj_ccallback.c
threading_source=$root/src/lib_threading.c
arm64_vm=$root/src/vm_arm64.dasc
x64_vm=$root/src/vm_x64.dasc
fixture_source=$root/tests/t-ffi-callback-auto-attach.c

line_of() {
  printf '%s\n' "$1" | grep -nF "$2" | head -n 1 | cut -d: -f1
}

arm64_cont=$(sed -n '/|->cont_ffi_callback:/,/|->vm_ffi_callback_dead:/p' \
  "$arm64_vm")
x64_cont=$(sed -n '/|->cont_ffi_callback:/,/|->vm_ffi_callback_dead:/p' \
  "$x64_vm")
leave_body=$(sed -n '/^static lua_State \*ccallback_leave(/,/^}/p' \
  "$callback_source")
finish_body=$(sed -n \
  '/^void LJ_FASTCALL lj_ccallback_leave_result_finish(/,/^}/p' \
  "$callback_source")
pending_body=$(sed -n \
  '/^void lj_threading_detach_callback_pending(/,/^}/p' \
  "$threading_source")

test -n "$arm64_cont" && test -n "$x64_cont" && \
  test -n "$leave_body" && test -n "$finish_body" && \
  test -n "$pending_body"

for required in \
  'lj_ccallback_leave_result' \
  'ldp BASE, PC, CBACK:KBASE->gpr[0]' \
  'ldp d8, d9, CBACK:KBASE->fpr[0]' \
  'lj_ccallback_leave_result_finish' \
  'mov x0, BASE' \
  'mov x1, PC' \
  'fmov d0, d8' \
  'fmov d1, d9'; do
  printf '%s\n' "$arm64_cont" | grep -F "$required" >/dev/null || {
    echo "ARM64 callback-result continuation lost: $required" >&2
    exit 1
  }
done
arm_leave=$(line_of "$arm64_cont" 'lj_ccallback_leave_result //')
arm_gpr=$(line_of "$arm64_cont" 'ldp BASE, PC, CBACK:KBASE->gpr[0]')
arm_fpr=$(line_of "$arm64_cont" 'ldp d8, d9, CBACK:KBASE->fpr[0]')
arm_finish=$(line_of "$arm64_cont" 'lj_ccallback_leave_result_finish')
arm_restore=$(line_of "$arm64_cont" 'mov x0, BASE')
test "$arm_leave" -lt "$arm_gpr" && test "$arm_gpr" -lt "$arm_finish" && \
  test "$arm_leave" -lt "$arm_fpr" && test "$arm_fpr" -lt "$arm_finish" && \
  test "$arm_finish" -lt "$arm_restore"
if printf '%s\n' "$arm64_cont" | sed -n "$((arm_finish + 1)),\$p" | \
   grep -F 'CBACK:' >/dev/null; then
  echo "ARM64 callback continuation rereads TG-owned results after detach" >&2
  exit 1
fi

for required in \
  'lj_ccallback_leave_result' \
  'mov PC, qword CBACK:KBASE->fpr[0]' \
  'mov KBASE, CBACK:KBASE->gpr[0]' \
  'lj_ccallback_leave_result_finish' \
  'mov rax, KBASE' \
  'movd xmm0, PC'; do
  printf '%s\n' "$x64_cont" | grep -F "$required" >/dev/null || {
    echo "x64 callback-result continuation lost: $required" >&2
    exit 1
  }
done
x64_leave=$(line_of "$x64_cont" 'lj_ccallback_leave_result //')
x64_gpr=$(line_of "$x64_cont" 'mov KBASE, CBACK:KBASE->gpr[0]')
x64_fpr=$(line_of "$x64_cont" 'mov PC, qword CBACK:KBASE->fpr[0]')
x64_finish=$(line_of "$x64_cont" 'lj_ccallback_leave_result_finish')
x64_restore=$(line_of "$x64_cont" 'mov rax, KBASE')
test "$x64_leave" -lt "$x64_gpr" && test "$x64_gpr" -lt "$x64_finish" && \
  test "$x64_leave" -lt "$x64_fpr" && test "$x64_fpr" -lt "$x64_finish" && \
  test "$x64_finish" -lt "$x64_restore"
if printf '%s\n' "$x64_cont" | sed -n "$((x64_finish + 1)),\$p" | \
   grep -F 'CBACK:' >/dev/null; then
  echo "x64 callback continuation rereads TG-owned results after detach" >&2
  exit 1
fi

pending_publish=$(line_of "$leave_body" 'ccallback_auto_detach_rel(cb, 1)')
error_restore=$(line_of "$leave_body" 'ccallback_error_restore(&err);')
retained_return=$(line_of "$leave_body" \
  'return auto_detach && retain_result ? L : NULL;')
test -n "$pending_publish" && test -n "$error_restore" && \
  test -n "$retained_return" && test "$pending_publish" -lt "$error_restore" && \
  test "$error_restore" -lt "$retained_return"

finish_save=$(line_of "$finish_body" 'ccallback_error_save(&err);')
finish_detach=$(line_of "$finish_body" \
  'lj_threading_detach_callback_pending(L);')
finish_hook=$(line_of "$finish_body" 'ccallback_test_after_detach();')
finish_restore=$(line_of "$finish_body" 'ccallback_error_restore(&err);')
test -n "$finish_save" && test -n "$finish_detach" && \
  test -n "$finish_hook" && test -n "$finish_restore" && \
  test "$finish_save" -lt "$finish_detach" && \
  test "$finish_detach" -lt "$finish_hook" && \
  test "$finish_hook" -lt "$finish_restore"
for forbidden in 'L->' 'G(L)' 'L2TG' 'tg->' 'cb->'; do
  if printf '%s\n' "$finish_body" | \
     sed -n "$((finish_detach + 1)),\$p" | grep -F "$forbidden" >/dev/null; then
    echo "callback result finish touches retired state after detach: $forbidden" >&2
    exit 1
  fi
done

for required in \
  'ccallback_auto_detach_acq(cb) != 1' \
  'ccallback_depth_acq(cb) != 0' \
  'ccallback_L_acq(cb) != NULL' \
  'ccallback_slot_acq(cb) != 0' \
  'ccallback_native_had_stopreq_acq(cb) != 0' \
  'lj_tg_ffi_call_func_acq(tg) != NULL' \
  'lj_tg_load_cur_L(tg) != L' \
  'lj_tg_load_thread_L(tg) != L' \
  'ccallback_auto_detach_rel(cb, 0);' \
  'threading_detach_scope_quiescent(g, tg, L)' \
  'threading_jit_detach_preabort_ready(g, tg)' \
  'ccallback_auto_detach_rel(cb, 1);' \
  'threading_detach_commit(L, tg, 0);'; do
  printf '%s\n' "$pending_body" | grep -F "$required" >/dev/null || {
    echo "callback pending-detach contract lost: $required" >&2
    exit 1
  }
done
pending_clear=$(line_of "$pending_body" 'ccallback_auto_detach_rel(cb, 0);')
pending_scope=$(line_of "$pending_body" \
  'threading_detach_scope_quiescent(g, tg, L)')
pending_jit=$(line_of "$pending_body" \
  'threading_jit_detach_preabort_ready(g, tg)')
pending_restore=$(line_of "$pending_body" \
  'ccallback_auto_detach_rel(cb, 1);')
pending_commit=$(line_of "$pending_body" \
  'threading_detach_commit(L, tg, 0);')
test "$pending_clear" -lt "$pending_scope" && \
  test "$pending_scope" -le "$pending_jit" && \
  test "$pending_jit" -lt "$pending_restore" && \
  test "$pending_restore" -lt "$pending_commit"

for required in \
  '^|[.]define KBASE,[[:space:]]+rdi' \
  '^|[.]define PC,[[:space:]]+rsi' \
  '^|[[:space:]]+push rdi; push rsi; push rbx' \
  '^|[.]define KBASE,[[:space:]]+r15' \
  '^|[.]define PC,[[:space:]]+rbx' \
  '^|[[:space:]]+push rbx; push r15; push r14'; do
  grep -E "$required" "$x64_vm" >/dev/null || {
    echo "x64 callback-result saved-register contract lost: $required" >&2
    exit 1
  }
done
for required in \
  'err->winerr = (uint32_t)GetLastError();' \
  'SetLastError((DWORD)err->winerr);'; do
  grep -F "$required" "$callback_source" >/dev/null || {
    echo "Win64 callback error-pair contract lost: $required" >&2
    exit 1
  }
done
for required in \
  'la_storefunc_rel(&ccallback_test_after_detach_hook, hook);' \
  'la_xchgfunc_acqrel(' \
  '&ccallback_test_after_detach_hook, NULL);'; do
  grep -F "$required" "$callback_source" >/dev/null || {
    echo "callback result test-hook atomic contract lost: $required" >&2
    exit 1
  }
done

for required in \
  'lj_ccallback_test_set_after_detach_hook(result_after_detach_hold);' \
  'assert(mt_live_acq(g) == 0);' \
  'assert(lj_tg_reclaim_dead(g) == 1u);' \
  'assert(intctx.result == UINT64_C(0xfedcba9876543210));' \
  'assert(fpctx.fp_result == 13.0);' \
  'assert(intctx.errnum == E2BIG);' \
  'assert(fpctx.errnum == EILSEQ);'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "callback result lifetime fixture lost: $required" >&2
    exit 1
  }
done

if test "${LJ_ARM64_CALLBACK_RESULT_SOURCE_ONLY:-0}" = 1; then
  echo "arm64_ffi_callback_result_lifetime_contract OK: source-only"
  exit 0
fi

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_ffi_callback_result_lifetime_contract SKIP: dynamic proof requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=
jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
ordinary_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
restore_xcflags=${LJ_CALLBACK_RESULT_RESTORE_XCFLAGS:-$ordinary_xcflags}
helper_define='-DLJ_CCALLBACK_TEST_HELPERS'

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if test "$restore_needed" = 1; then
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" clean TARGET_SYS=Darwin \
        TARGET_FLAGS='-arch arm64' XCFLAGS="$restore_xcflags" \
        >/dev/null 2>&1 || status=1
    env MACOSX_DEPLOYMENT_TARGET="$minver" \
      make -C "$root/src" -j"$jobs" TARGET_SYS=Darwin \
        TARGET_FLAGS='-arch arm64' XCFLAGS="$restore_xcflags" \
        >/dev/null 2>&1 || status=1
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
trap cleanup EXIT HUP INT TERM

if test "${LJ_TEST_DISABLE_RUN_LOCK:-0}" != 1 && \
   test "${LJ_TEST_RUN_LOCK_HELD:-0}" != 1; then
  while ! mkdir "$lock_dir" 2>/dev/null; do sleep 0.2; done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-callback-result.XXXXXX")
restore_needed=1
cc=${CC:-clang}

run_case() {
  tag=$1
  target_flags=$2
  xcflags=$3
  expected_arch=$4
  fixture=$tmpdir/t-ffi-callback-result-$tag

  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" clean TARGET_SYS=Darwin \
      TARGET_FLAGS="$target_flags" XCFLAGS="$xcflags"
  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" -j"$jobs" TARGET_SYS=Darwin \
      TARGET_FLAGS="$target_flags" XCFLAGS="$xcflags"
  test "$(lipo -archs "$root/src/libluajit.a")" = "$expected_arch"
  test "$(lipo -archs "$root/src/lj_vm.o")" = "$expected_arch"
  nm -u "$root/src/lj_vm.o" | \
    grep '_lj_ccallback_leave_result$' >/dev/null
  nm -u "$root/src/lj_vm.o" | \
    grep '_lj_ccallback_leave_result_finish$' >/dev/null
  if nm -u "$root/src/lj_vm.o" | \
     grep '_lj_ccallback_leave$' >/dev/null; then
    echo "$tag VM still uses eager callback leave" >&2
    exit 1
  fi

  # shellcheck disable=SC2086 # Target/compiler flag groups are intentional.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
    "$fixture_source" "$root/src/libluajit.a" -lm -pthread -o "$fixture"
  run=0
  while test "$run" -lt "${LJ_CALLBACK_RESULT_RUNS:-2}"; do
    env MallocScribble=1 "$fixture"
    run=$((run + 1))
  done
}

arm64_flags="$ordinary_xcflags $helper_define"
arm64e_flags="$arm64_flags -DLUAJIT_ENABLE_CET_BR"
x64_flags="-DLUA_USE_ASSERT $helper_define"
run_case arm64 '-arch arm64' "$arm64_flags" arm64
run_case arm64e '-arch arm64e -mbranch-protection=bti' \
  "$arm64e_flags" arm64e
otool -hv "$tmpdir/t-ffi-callback-result-arm64e" | \
  grep -E 'ARM64[[:space:]]+E' >/dev/null
run_case x86_64 '-arch x86_64' "$x64_flags" x86_64

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_SYS=Darwin \
    TARGET_FLAGS='-arch arm64' XCFLAGS="$restore_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_SYS=Darwin \
    TARGET_FLAGS='-arch arm64' XCFLAGS="$restore_xcflags"
restore_needed=0

echo "arm64_ffi_callback_result_lifetime_contract OK: arm64, arm64e/BTI and x86_64 copied 64-bit integer/FP results across physical TG reclaim"
