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
xcflags=${LJ_ARM64_FUNCF_XCFLAGS:--DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLUAJIT_MCODE_TEST}
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
fixture_source=$root/tests/t-arm64-jit-funcf-record.c
trace_source=$root/src/lj_trace.c
asm_source=$root/src/lj_asm.c
admit_source=$root/src/lj_asm_arm64_admit.h
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
funcf_entry_postra=$tmpdir/funcf-entry-postra.txt
funcf_entry_generation=$tmpdir/funcf-entry-generation.txt
funcf_root_view=$tmpdir/funcf-root-view.txt
source_gate=$tmpdir/source-gate.txt
jfuncf_source=$tmpdir/jfuncf-source.txt
pauth_fixture=$tmpdir/t-arm64-jit-funcf-record-arm64e
pauth_obj=$tmpdir/t-arm64-jit-funcf-record-arm64e.o
pauth_disasm=$tmpdir/t-arm64-jit-funcf-record-arm64e.disasm
pauth_macros=$tmpdir/macros-arm64e.txt
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm
pauth_jfuncf_disasm=$tmpdir/vm-arm64e-jfuncf.disasm
restore_needed=1

# The C fixture itself exercises actual native publication for each supported
# scalar operation and value class, plus fail-closed mutations and entry races.
# Keep these checks about the evidence being run, not one particular IR layout.
for required in \
  'run_scalar_positives();' \
  'run_rejections();' \
  'run_integer_addov_native();' \
  '__arm64_funcf_false' \
  '__arm64_funcf_nil' \
  '__arm64_funcf_number' \
  '__arm64_funcf_argument' \
  '__arm64_funcf_add' \
  '__arm64_funcf_sub' \
  '__arm64_funcf_mul' \
  '__arm64_funcf_div' \
  '__arm64_funcf_multi' \
  'test_funcf_generation_race' \
  'ir[load].op1++' \
  'ir[result].prev = ir[arg].prev' \
  'ir[right].prev = ir[left].prev' \
  'assert(!lj_asm_arm64_postra_funcf_entry_admit(&view, live, NULL))' \
  'TRACE_ARM64_TRUE_FUNCF_ADMITTED' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA' \
  'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION' \
  'test_funcf_native_xpoll' \
  'test_funcf_native_stopreq'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 FUNCF executable proof lost required case: $required" >&2
    exit 1
  }
done

# Both independent validators derive the returned scalar from bytecode
# definitions and validate concrete snapshots/allocator state. Runtime
# positives and post-RA corruption/recovery execute these paths; source checks
# only ensure neither independent walker disappeared.
for required in \
  'arm64_ir_funcf_advance' \
  'arm64_ir_funcf_result_matches' \
  'arm64_ir_funcf_scalar_snapshots' \
  'arm64_postra_funcf_admit' \
  'arm64_scalar_postra_register' \
  'arm64_ir_funcf_bytecode'; do
  grep -F "$required" "$admit_source" >/dev/null || {
    echo "ARM64 FUNCF operation validator lost required proof: $required" >&2
    exit 1
  }
done

# Reject regressions to the former exact-three-word true-return template.
if grep -E 'pt->sizebc != 3|T->nins != REF_BASE\+3u|nsnap != 2 \|\| nsnapmap != 5|SNAP\(result_slot, 0, REF_TRUE\)' \
     "$admit_source" "$trace_source" >/dev/null; then
  echo "ARM64 FUNCF operation admission reverted to the exact-true template" >&2
  exit 1
fi

for required in \
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0'; do
  setting=${required%% *}
  value=${required##* }
  grep -E "^#define[[:space:]]+$setting[[:space:]]+$value$" \
    "$arch_source" >/dev/null || {
    echo "ARM64 FUNCF gate mismatch: $required" >&2
    exit 1
  }
done

# Native JFUNCF entry remains a separate VM boundary proof. The executable
# fixture checks both entry lifecycle and post-ADMISSION STOPREQ behavior.
grep -F 'trace_arm64_entrybc_matches' "$trace_source" >/dev/null
grep -F 'lj_asm_arm64_postra_funcf_entry_admit(' "$trace_source" >/dev/null
grep -F 'sourceins == v->startins' "$trace_source" >/dev/null
grep -F '&& prepublication)' "$trace_source" >/dev/null
grep -F 'sourceop == (uint32_t)BC_JFUNCF' "$trace_source" >/dev/null
grep -F 'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' "$vm_source" >/dev/null
awk '/case BC_JFUNCF:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /case BC_FUNCV:/ { exit }' \
  "$vm_source" >"$jfuncf_source"
for required in \
  'bl extern lj_trace_enter_root' \
  'sub sp, sp, #16' \
  'br_trace_auth CARG2, CRET1'; do
  grep -F "$required" "$jfuncf_source" >/dev/null || {
    echo "ARM64 JFUNCF native boundary lost required instruction: $required" >&2
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
  'LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0'; do
  name=${setting%% *}
  value=${setting##* }
  grep -E "^#define[[:space:]]+$name[[:space:]]+$value$" \
    "$macros" >/dev/null || {
    echo "ARM64 FUNCF profile mismatch: $setting" >&2
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
     copy && /^_lj_BC_(FUNCV|IFUNCV):/ { exit }' \
  "$vm_disasm" >"$jfuncf_disasm"
test -s "$jfuncf_disasm"
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jfuncf_disasm" >/dev/null
grep -E 'br[[:space:]]+x1$' "$jfuncf_disasm" >/dev/null
if grep -E 'braa[[:space:]]+x1, x0$' "$jfuncf_disasm" >/dev/null; then
  echo "ordinary JFUNCF unexpectedly uses an authenticated transfer" >&2
  exit 1
fi

ordinary_runs=${LJ_ARM64_FUNCF_RECORD_RUNS:-3}
run=1
while test "$run" -le "$ordinary_runs"; do
  "$fixture" default
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$fixture" randomized

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
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0'; do
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
     copy && /^_lj_BC_(FUNCV|IFUNCV):/ { exit }' \
  "$pauth_vm_disasm" >"$pauth_jfuncf_disasm"
test -s "$pauth_jfuncf_disasm"
grep -E 'bti[[:space:]]+j' "$pauth_jfuncf_disasm" >/dev/null
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$pauth_jfuncf_disasm" >/dev/null
grep -E 'braa[[:space:]]+x1, x0$' "$pauth_jfuncf_disasm" >/dev/null
if grep -E 'br[[:space:]]+x1$|braaz[[:space:]]+x1$' \
     "$pauth_jfuncf_disasm" >/dev/null; then
  echo "arm64e JFUNCF lost its trace-discriminated transfer" >&2
  exit 1
fi

pauth_runs=${LJ_ARM64_FUNCF_RECORD_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  "$pauth_fixture" default
  run=$((run+1))
done
LUAJIT_MCODE_TEST=R "$pauth_fixture" randomized

# Leave the shared checkout in the ordinary experimental configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_funcf_record_contract OK: scalar FUNCF proofs passed natively on ARM64/ARM64e"
