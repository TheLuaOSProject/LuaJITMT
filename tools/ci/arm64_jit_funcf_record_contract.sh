#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_funcf_record_contract SKIP: requires native macOS arm64"
  exit 0
fi

if test -z "${SDKROOT:-}"; then
  SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
  export SDKROOT
fi

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-$(xcrun --sdk macosx --find clang)}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
fixture_source=$root/tests/t-arm64-jit-funcf-record.c
trace_source=$root/src/lj_trace.c
asm_source=$root/src/lj_asm.c
arch_source=$root/src/lj_arch.h
jit_header=$root/src/lj_jit.h
vm_source=$root/src/vm_arm64.dasc
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
        XCFLAGS="$xcflags" >/dev/null 2>&1 || restore_status=$?
    if test "$restore_status" = 0; then
      env MACOSX_DEPLOYMENT_TARGET="$minver" \
        make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
          XCFLAGS="$xcflags" >/dev/null 2>&1 || restore_status=$?
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
    echo "ARM64 FUNCF record contract could not restore arm64 build" >&2
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
      echo "ARM64 FUNCF record contract lock timed out: $lock_dir" >&2
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
}

acquire_lock
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-funcf-record.XXXXXX")
fixture=$tmpdir/t-arm64-jit-funcf-record
fixture_obj=$tmpdir/t-arm64-jit-funcf-record.o
fixture_disasm=$tmpdir/t-arm64-jit-funcf-record.disasm
macros=$tmpdir/macros-arm64.txt
vm_disasm=$tmpdir/vm-arm64.disasm
jfuncf_disasm=$tmpdir/vm-arm64-jfuncf.disasm
vm_reloc=$tmpdir/vm-arm64.reloc
funcf_generation=$tmpdir/funcf-generation.txt
funcf_bytecode=$tmpdir/funcf-bytecode.txt
funcf_shape=$tmpdir/funcf-shape.txt
funcf_postra=$tmpdir/funcf-postra.txt
source_gate=$tmpdir/source-gate.txt
jfuncf_source=$tmpdir/jfuncf-source.txt
pauth_fixture=$tmpdir/t-arm64-jit-funcf-record-arm64e
pauth_obj=$tmpdir/t-arm64-jit-funcf-record-arm64e.o
pauth_disasm=$tmpdir/t-arm64-jit-funcf-record-arm64e.disasm
pauth_macros=$tmpdir/macros-arm64e.txt
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm
pauth_jfuncf_disasm=$tmpdir/vm-arm64e-jfuncf.disasm
restore_needed=1

# Freeze the runtime fixture's publication-only boundary. The hotcall is made
# through lua_pcall with both parameters missing, then JFUNCF calls with zero,
# one and two arguments must all recover through the interpreter.
for required in \
  'function __arm64_funcf_true(a, b) return true end' \
  'trace_link_acq(T) == 0' \
  'trace_linktype_acq(T) == LJ_TRLINK_RETURN' \
  'flags == TRACE_ARM64_TRUE_FUNCF_ADMITTED' \
  'trace_mcloop_acq(T) == 0' \
  'trace_spadjust_acq(T) == 0' \
  'FUNCF_R_SEPARATOR, IR_NOP, IRT_NIL, 0, 0' \
  'FUNCF_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0' \
  'trace_nsnap_acq(T) == 2' \
  'trace_nsnapmap_acq(T) == 5' \
  'SNAP(result_slot, 0, REF_TRUE)' \
  'expect_closed_jfuncf_boundary(L, 1)' \
  'expect_closed_jfuncf_boundary(L, 2)' \
  'expect_closed_jfuncf_boundary(L, 4)' \
  'lj_trace_test_root_entry_publishes() == 0' \
  'lj_trace_test_root_entry_cleanups() == 0' \
  'lj_tg_load_jit_base(tg) == NULL' \
  'run_lua(L, "jit.flush(); jit.opt.start('\''hotloop=1'\'')\n");' \
  'trace_traceno_acq(T) == 1' \
  '__arm64_funcf_false' \
  '__arm64_funcf_nil' \
  '__arm64_funcf_number' \
  '__arm64_funcf_nontrivial'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 FUNCF fixture lost required proof: $required" >&2
    exit 1
  }
done

# Recorder ingress validates one immutable three-word header generation, and
# does not share the LOOP/FORL start geometry.
awk '/^static int trace_root_funcf_shape/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$funcf_generation"
for required in \
  'pt->sizebc != 3' \
  '(pt->flags & PROTO_VARARG) != 0' \
  'bc_op(startins) != BC_FUNCF' \
  'bc_a(startins) != pt->framesize' \
  'bc_d(startins) != 0' \
  'pt->numparams > result' \
  'bc_op(kpri) == BC_KPRI' \
  'bc_d(kpri) == 2u' \
  'bc_op(ret) == BC_RET1' \
  'bc_d(ret) == 2u' \
  'la_load32_acq((const uint32_t *)&bc[0]) == startins' \
  'la_load32_acq((const uint32_t *)&bc[1]) == kpri' \
  'la_load32_acq((const uint32_t *)&bc[2]) == ret'; do
  grep -F "$required" "$funcf_generation" >/dev/null || {
    echo "ARM64 FUNCF recorder generation proof changed: $required" >&2
    exit 1
  }
done
test "$(grep -Fc 'trace_root_funcf_shape(' "$trace_source")" -eq 2

# The assembler independently repeats the bytecode proof and admits only the
# separator NOP plus terminal XPOLL(1), the exact primitive snapshot, and a
# spill-free allocator suffix.
awk '/^static int arm64_ir_funcf_bytecode/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$asm_source" >"$funcf_bytecode"
for required in \
  'sizebc != 3' \
  '(lo & (sizeof(BCIns)-1)) != 0' \
  '(UINTPTR_MAX-lo)/sizeof(BCIns)' \
  'bc_op(startins) != BC_FUNCF' \
  'bc_d(kpri) == 2u' \
  'bc_op(ret) == BC_RET1' \
  'bc_d(ret) == 2u' \
  'la_load32_acq((const uint32_t *)&bc[0]) == startins' \
  'la_load32_acq((const uint32_t *)&bc[1]) == kpri' \
  'la_load32_acq((const uint32_t *)&bc[2]) == ret'; do
  grep -F "$required" "$funcf_bytecode" >/dev/null || {
    echo "ARM64 FUNCF assembler bytecode proof changed: $required" >&2
    exit 1
  }
done
awk '/^static int arm64_ir_funcf_shape/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$asm_source" >"$funcf_shape"
for required in \
  'J->loopref != 0' \
  'T->nins != REF_BASE+3u' \
  'T->nk != REF_TRUE' \
  'ir[REF_BASE+1u].o != IR_NOP' \
  'ir[REF_BASE+2u].o != IR_XPOLL' \
  'ir[REF_BASE+2u].op1 != 1' \
  'arm64_ir_funcf_snapshots'; do
  grep -F "$required" "$funcf_shape" >/dev/null || {
    echo "ARM64 FUNCF semantic shape changed: $required" >&2
    exit 1
  }
done
awk '/^static int arm64_postra_funcf_admit/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$asm_source" >"$funcf_postra"
for required in \
  'view->nins != REF_BASE+4u' \
  'view->spadjust != 0' \
  'entry.o != IR_NOP' \
  'poll.o != IR_XPOLL' \
  'poll.op1 != 1' \
  'suffix.o != IR_NOP' \
  'suffix.prev != 0' \
  '*semantic_ninsp = REF_BASE+3u'; do
  grep -F "$required" "$funcf_postra" >/dev/null || {
    echo "ARM64 FUNCF post-RA shape changed: $required" >&2
    exit 1
  }
done
for required in \
  'nsnap != 2 || nsnapmap != 5' \
  'snapentry_acq(&snapmap[2]) != SNAP(result_slot, 0, REF_TRUE)' \
  'snappos != 1u' \
  'snappos == 2u'; do
  grep -F "$required" "$asm_source" >/dev/null || {
    echo "ARM64 FUNCF snapshot proof changed: $required" >&2
    exit 1
  }
done

grep -E '^#define TRACE_ARM64_TRUE_FUNCF_ADMITTED[[:space:]]+0x40$' \
  "$jit_header" >/dev/null
grep -F 'T->unused1 |= TRACE_ARM64_TRUE_FUNCF_ADMITTED;' \
  "$asm_source" >/dev/null

# Native entry remains independently closed. The C helper returns before the
# jit_base publication point, and the checked-in VM clears any impossible
# successful lease instead of branching to the generated mcode.
awk '/^static LJ_AINLINE int trace_root_entry_source_admitted/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$trace_source" >"$source_gate"
grep -F 'sourceop == (uint32_t)BC_JFUNCF' "$source_gate" >/dev/null
grep -F '!LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' \
  "$source_gate" >/dev/null
awk '/case BC_JFUNCF:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /^  case BC_JFUNCV:/ { exit }' \
  "$vm_source" >"$jfuncf_source"
for required in \
  'bl extern lj_trace_enter_root' \
  '#if LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' \
  'clear_tg_jit_base' \
  'br_trace_auth CARG2, CRET1' \
  'bl extern lj_trace_stale_startins' \
  'Preserve callee-save RC'; do
  grep -F "$required" "$jfuncf_source" >/dev/null || {
    echo "ARM64 JFUNCF source boundary changed: $required" >&2
    exit 1
  }
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 0' \
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -E "^#define ${setting}$" "$macros" >/dev/null || {
    echo "ARM64 FUNCF gate mismatch: $setting" >&2
    exit 1
  }
done

test "$(lipo -archs "$archive")" = arm64
nm "$vm_object" | grep ' U _lj_trace_enter_root$' >/dev/null
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+ALL' >/dev/null
otool -tvV "$fixture_obj" >"$fixture_disasm"
grep '^_main:' "$fixture_disasm" >/dev/null

otool -rv "$vm_object" >"$vm_reloc"
grep 'BR26.*_lj_trace_enter_root$' "$vm_reloc" >/dev/null
otool -tvV "$vm_object" >"$vm_disasm"
awk '/^_lj_BC_JFUNCF:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_FUNCV:/ { exit }' \
  "$vm_disasm" >"$jfuncf_disasm"
test -s "$jfuncf_disasm"
grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$jfuncf_disasm" >/dev/null
if grep -E '(br[[:space:]]+x1|braa[[:space:]]+x1, x0)$' \
     "$jfuncf_disasm" >/dev/null; then
  echo "closed ordinary JFUNCF unexpectedly transfers to mcode" >&2
  exit 1
fi
if grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jfuncf_disasm" >/dev/null; then
  echo "publication-only ordinary JFUNCF unexpectedly reserves native frame" >&2
  exit 1
fi

ordinary_runs=${LJ_ARM64_FUNCF_RECORD_RUNS:-3}
run=1
while test "$run" -le "$ordinary_runs"; do
  "$fixture"
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$fixture"

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
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -E "^#define ${setting}$" "$pauth_macros" >/dev/null || {
    echo "ARM64e FUNCF gate mismatch: $setting" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null
otool -tvV "$pauth_obj" >"$pauth_disasm"
grep '^_main:' "$pauth_disasm" >/dev/null
otool -tvV "$vm_object" >"$pauth_vm_disasm"
awk '/^_lj_BC_JFUNCF:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_FUNCV:/ { exit }' \
  "$pauth_vm_disasm" >"$pauth_jfuncf_disasm"
test -s "$pauth_jfuncf_disasm"
grep -E 'bti[[:space:]]+j' "$pauth_jfuncf_disasm" >/dev/null
grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$pauth_jfuncf_disasm" >/dev/null
if grep -E '(br[[:space:]]+x1|braa[[:space:]]+x1, x0)$' \
     "$pauth_jfuncf_disasm" >/dev/null; then
  echo "closed arm64e JFUNCF unexpectedly transfers to authenticated mcode" >&2
  exit 1
fi
if grep -E 'sub[[:space:]]+sp, sp, #0x10' \
     "$pauth_jfuncf_disasm" >/dev/null; then
  echo "publication-only arm64e JFUNCF unexpectedly reserves native frame" >&2
  exit 1
fi

pauth_runs=${LJ_ARM64_FUNCF_RECORD_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  "$pauth_fixture"
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$pauth_fixture"

# Leave the shared checkout in the ordinary experimental configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_funcf_record_contract OK: exact true FUNCF published on ARM64/ARM64e; native JFUNCF entry stayed closed"
