#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_exit_contract SKIP: requires native macOS arm64"
  exit 0
fi

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
trap cleanup EXIT HUP INT TERM
if test "${LJ_TEST_DISABLE_RUN_LOCK:-}" != 1 &&
   test "${LJ_TEST_RUN_LOCK_HELD:-}" != 1; then
  while ! mkdir "$lock_dir" 2>/dev/null; do sleep 0.2; done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-jit-exit.XXXXXX")

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLJ_ARM64_EXIT_TEST_HELPERS'
archive=$root/src/libluajit.a
fixture=$tmpdir/t-arm64-jit-exit
live_fixture=$tmpdir/t-arm64-jit-exittab
live_trap_object=$tmpdir/t-arm64-jit-exittab-trap.o
trace_retire_fixture=$tmpdir/t-jit-trace-retire
stub_words=$tmpdir/exit-stubs.bin
empty_object=$tmpdir/empty.o
stub_object=$tmpdir/exit-stubs.o
stub_disasm=$tmpdir/exit-stubs.disasm
stub_code_disasm=$tmpdir/exit-stubs-code.disasm
vm_disasm=$tmpdir/vm.disasm
jloop_region=$tmpdir/vm-jloop.txt
handler_region=$tmpdir/vm-exit-handler.txt
handler_ingress_region=$tmpdir/vm-exit-handler-ingress.txt
interp_region=$tmpdir/vm-exit-interp.txt
stale_dispatch_region=$tmpdir/vm-exit-stale-dispatch.txt
trace_region=$tmpdir/trace-exit.txt
patchexit_region=$tmpdir/asm-patchexit.txt
exitstub_writer_region=$tmpdir/asm-exitstub-write.txt
exitstub_setup_region=$tmpdir/asm-exitstub-setup.txt
asm_mclimit_region=$tmpdir/asm-mclimit.txt
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
pauth_fixture=$tmpdir/t-arm64-jit-exit-arm64e
pauth_live_fixture=$tmpdir/t-arm64-jit-exittab-arm64e
pauth_live_trap_object=$tmpdir/t-arm64-jit-exittab-trap-arm64e.o
pauth_trace_retire_fixture=$tmpdir/t-jit-trace-retire-arm64e
pauth_words=$tmpdir/exit-stubs-arm64e.bin
pauth_empty=$tmpdir/empty-arm64e.o
pauth_stub_object=$tmpdir/exit-stubs-arm64e.o
pauth_stub_disasm=$tmpdir/exit-stubs-arm64e.disasm
pauth_stub_code_disasm=$tmpdir/exit-stubs-arm64e-code.disasm
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm
pauth_jloop_region=$tmpdir/vm-arm64e-jloop.txt
pauth_handler_ingress_region=$tmpdir/vm-arm64e-exit-handler-ingress.txt
pauth_macros=$tmpdir/macros-arm64e.txt
restore_needed=1

# Extract only the four fallback instructions and the six instruction words
# in each gate. The last two words of every gate are a runtime slot-address
# literal and must not be interpreted as code by mnemonic-count checks.
extract_exit_code() {
  disasm=$1
  output=$2
  : >"$output"
  word=0
  while test "$word" -lt 4; do
    address=$(printf '%016x' $((word * 4)))
    grep -E "^[[:space:]]*$address[[:space:]]" "$disasm" >>"$output"
    word=$((word + 1))
  done
  gate=0
  while test "$gate" -lt 4; do
    word=0
    while test "$word" -lt 6; do
      address=$(printf '%016x' $((16 + gate * 32 + word * 4)))
      grep -E "^[[:space:]]*$address[[:space:]]" "$disasm" >>"$output"
      word=$((word + 1))
    done
    gate=$((gate + 1))
  done
  test "$(wc -l <"$output" | tr -d '[:space:]')" = 28
}

if grep -F 'LJ_ARM64_JIT_NATIVE_ENTRY_FAIL_CLOSED' \
     "$root/tests/t-arm64-jit-exit.c" >/dev/null; then
  echo "ARM64 exit fixture still depends on the aggregate native-entry gate" >&2
  exit 1
fi
for setting in \
  'LJ_ABI_PAUTH' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED'; do
  grep -F "$setting" "$root/tests/t-arm64-jit-exit.c" >/dev/null || {
    echo "ARM64 exit fixture lost granular setting $setting" >&2
    exit 1
  }
done

for primitive in \
  'la_xchg8_acqrel' \
  'snap_topslot_cas_acqrel' \
  'snap_count_xchg_acqrel' \
  'trace_nextside_cas_acqrel' \
  'trace_exittarget_arm64_raw_cas_acqrel' \
  'pointer_bits(la_loadptr_acq' \
  'test_first_child_publication_primitives(L);'; do
  grep -F "$primitive" "$root/src/lj_atomic.h" "$root/src/lj_jit.h" \
    "$root/tests/t-arm64-jit-exit.c" >/dev/null || {
      echo "ARM64 first-child publication primitive missing: $primitive" >&2
      exit 1
    }
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_asm_arm64_exitstub_test$' >/dev/null

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-exit.c" "$archive" -lm -pthread -o "$fixture"
"$fixture" "$stub_words"
test "$(wc -c <"$stub_words" | tr -d '[:space:]')" = 144

"$cc" -arch arm64 -mmacosx-version-min="$minver" -x assembler \
  -c /dev/null -o "$empty_object"
ld -r -arch arm64 -o "$stub_object" "$empty_object" \
  -sectcreate __TEXT __text "$stub_words"
otool -tvV "$stub_object" >"$stub_disasm"
extract_exit_code "$stub_disasm" "$stub_code_disasm"

# One ordinary slice: a placement-invariant K64 fallback followed by four
# immutable gates. Every gate publishes its exit number, acquire-loads the
# heap slot, and branches without touching executable code.
test "$(grep -Ec 'bti[[:space:]]+j$' "$stub_code_disasm")" = 1
test "$(grep -Ec 'ldr[[:space:]]+x30, \[x22(, #0x[[:xdigit:]]+)?\]' \
  "$stub_code_disasm")" = 1
test "$(grep -Ec 'blr[[:space:]]+x30$' "$stub_code_disasm")" = 1
test "$(grep -Ec 'mov[[:space:]]+w0, #0x1234$' \
  "$stub_code_disasm")" = 1
test "$(grep -Ec 'mov[[:space:]]+w30, #0x[[:xdigit:]]+$' \
  "$stub_code_disasm")" = 4
test "$(grep -Ec 'str[[:space:]]+x30, \[sp\]$' \
  "$stub_code_disasm")" = 4
test "$(grep -Ec 'ldar[[:space:]]+x30, \[x30\]$' \
  "$stub_code_disasm")" = 4
test "$(grep -Ec 'br[[:space:]]+x30$' "$stub_code_disasm")" = 4
test "$(grep -Ec 'nop$' "$stub_code_disasm")" = 4
test "$(grep -Ec 'braa|blraaz|[[:space:]]bl[[:space:]]' \
  "$stub_code_disasm" || true)" = 0

otool -tvV "$root/src/lj_vm.o" >"$vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' "$vm_disasm" >"$jloop_region"
awk '/^_lj_vm_exit_handler:/ { copy=1 }
     copy { print }
     copy && /^_lj_vm_exit_interp:/ { exit }' "$vm_disasm" >"$handler_region"
awk '/^_lj_vm_exit_handler:/ { copy=1 }
     copy { print }
     copy && /[[:space:]]bl[[:space:]]/ { exit }' \
  "$vm_disasm" >"$handler_ingress_region"
awk '/^_lj_vm_exit_interp:/ { copy=1 }
     copy { print }
     copy && /^_lj_vm_modi:/ { exit }' "$vm_disasm" >"$interp_region"
test -s "$jloop_region" && test -s "$handler_region" && \
  test -s "$handler_ingress_region" && test -s "$interp_region"
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jloop_region" >/dev/null
test "$(grep -Ec 'br[[:space:]]+x1$' "$jloop_region")" = 1
grep -E 'add[[:space:]]+x2, sp, #0x200' \
  "$handler_ingress_region" >/dev/null
grep -E 'ldr[[:space:]]+w0, \[x2\]' "$handler_ingress_region" >/dev/null
grep -E 'ldr[[:space:]]+w1, \[x30\]' "$handler_ingress_region" >/dev/null
if grep -E 'ldr[[:space:]]+x0, \[sp, #0x1f8\]|sub[[:space:]]+x0,|lsr[[:space:]]+w0,' \
     "$handler_ingress_region" >/dev/null; then
  echo "ARM64 exit handler still derives exit number from the saved LR" >&2
  exit 1
fi
grep -E 'add[[:space:]]+x14, x25' "$handler_region" >/dev/null
grep -E 'ldar[[:space:]]+x19, \[x14\]' "$handler_region" >/dev/null
if grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$handler_region" >/dev/null; then
  echo "ARM64 exit handler clears the TG trace lease before C restore" >&2
  exit 1
fi
test "$(grep -Ec 'stlr[[:space:]]+xzr, \[x14\]' "$interp_region")" = 2
test "$(grep -Ec 'ldar[[:space:]]+w8, \[x14\]' "$interp_region")" = 2
test "$(grep -Ec 'ldar[[:space:]]+w9, \[x14\]' "$interp_region")" = 2
awk '/ldar[[:space:]]+w16, \[x14\]/ { copy=1 }
     copy { print }
     copy && /br[[:space:]]+x17$/ { exit }' \
  "$interp_region" >"$stale_dispatch_region"
test -s "$stale_dispatch_region"
stale_branch_target=$(awk '$2 == "b.ne" { sub(/^0x/, "", $3); print $3; exit }' \
  "$stale_dispatch_region")
stale_dispatch_addr=$(awk '
  $2 == "add" && $3 == "x8," && $4 == "x22," && $5 == "w16," {
    addr = $1
    sub(/^0+/, "", addr)
    print addr
    exit
  }' "$stale_dispatch_region")
test -n "$stale_branch_target" && test -n "$stale_dispatch_addr"
test "$stale_branch_target" = "$stale_dispatch_addr" || {
  echo "ARM64 stale JLOOP replacement branch skips current static dispatch" >&2
  exit 1
}
grep -E 'ldr[[:space:]]+x17, \[x8, #0x[[:xdigit:]]+\]' \
  "$stale_dispatch_region" >/dev/null
grep -E 'br[[:space:]]+x17$' "$stale_dispatch_region" >/dev/null

awk '/^int LJ_FASTCALL lj_trace_exit\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$trace_region"
test -s "$trace_region"
grep -F 'TGState *tg = G2TG(G(L));' "$trace_region" >/dev/null
test "$(grep -Fc 'lj_tg_jit_exitcode_acq(tg)' "$trace_region")" = 1
test "$(grep -Fc 'lj_tg_jit_exitcode_rel(tg, 0)' "$trace_region")" = 1
test "$(grep -Fc 'lj_tg_store_jit_base(tg, NULL)' "$trace_region")" = 1

handler_source=$tmpdir/vm-exit-handler.dasc
handler_ingress_source=$tmpdir/vm-exit-handler-ingress.dasc
interp_source=$tmpdir/vm-exit-interp.dasc
awk '/[|]->vm_exit_handler:/ { copy=1 }
     copy { print }
     copy && /[|]->vm_exit_interp:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$handler_source"
awk '/[|]->vm_exit_handler:/ { copy=1 }
     copy { print }
     copy && /bl extern lj_trace_exit/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$handler_ingress_source"
awk '/[|]->vm_exit_interp:/ { copy=1 }
     copy { print }
     copy && /[|]->vm_modi:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$interp_source"
grep -F 'ld_tg_jit_base BASE' "$handler_source" >/dev/null
grep -F 'add CARG3, sp, #64*8' "$handler_ingress_source" >/dev/null
grep -F 'ldr CARG1w, [CARG3, #0]' "$handler_ingress_source" >/dev/null
grep -F 'ldr CARG2w, [lr]' "$handler_ingress_source" >/dev/null
for forbidden in \
  'ldr CARG1, [sp, #63*8]' \
  'sub CARG1, CARG1, CARG3' \
  'lsr CARG1w, CARG1w' \
  'sub CARG1w, CARG1w, #1'; do
  if grep -F "$forbidden" "$handler_ingress_source" >/dev/null; then
    echo "ARM64 exit handler still contains saved-LR exit arithmetic: $forbidden" >&2
    exit 1
  fi
done
if grep -F 'GL->jit_base' "$handler_source" "$interp_source" >/dev/null; then
  echo "ARM64 native exit still references global jit_base" >&2
  exit 1
fi
test "$(grep -Fc 'clear_tg_jit_base' "$interp_source")" = 2
test "$(grep -Fc 'arm64_vm_poll_acq' "$interp_source")" = 2
test "$(grep -Fc 'bl extern lj_safepoint_ack_check' "$interp_source")" = 2
for required in \
  'sub ATMP, PC, #4' \
  'ldar INSw, [ATMP]' \
  'cmp TMP0w, #BC_JLOOP' \
  'bne >6' \
  '6:  // A concurrent non-JLOOP replacement executes at this restored PC.' \
  'add TMP0, GL, INS, uxtb #3' \
  'ldr RB, [TMP0, #GG_G2DISP+GG_DISP2STATIC]' \
  'br_auth RB'; do
  grep -F "$required" "$interp_source" >/dev/null || {
    echo "ARM64 stale JLOOP recovery lost current-instruction dispatch: $required" >&2
    exit 1
  }
done
grep -F '#if LJ_TARGET_X64 || LJ_TARGET_ARM64' "$root/src/lj_err.c" >/dev/null
grep -F 'lj_tg_jit_exitcode_rel(G2TG(g), errcode);' \
  "$root/src/lj_err.c" >/dev/null

grep -E '^#define ARM64_EXIT_FALLBACK_WORDS[[:space:]]+4u$' \
  "$root/src/lj_target_arm64.h" >/dev/null
grep -E '^#define ARM64_EXIT_GATE_WORDS[[:space:]]+8u$' \
  "$root/src/lj_target_arm64.h" >/dev/null
grep -E '^#define LJ_ARM64_JIT_EXIT_TARGET_SLOTS[[:space:]]+1$' \
  "$root/src/lj_arch.h" >/dev/null
grep -E '^#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
  "$root/src/lj_arch.h" >/dev/null

awk '/^static void asm_exitstub_write\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' \
  "$root/src/lj_asm_arm64.h" >"$exitstub_writer_region"
test -s "$exitstub_writer_region"
grep -F 'glofs(as, &as->J->k64[LJ_K64_VM_EXIT_HANDLER])' \
  "$exitstub_writer_region" >/dev/null
grep -F 'fallback[0] = A64I_LE(A64I_BTI_J);' \
  "$exitstub_writer_region" >/dev/null
grep -F 'fallback[1] = A64I_LE(A64I_LDRx' \
  "$exitstub_writer_region" >/dev/null
grep -F 'A64F_N(RID_GL)' "$exitstub_writer_region" >/dev/null
grep -F 'fallback[2] = A64I_LE(A64I_BLR_AUTH' \
  "$exitstub_writer_region" >/dev/null
grep -F 'as->mcexit = gates;' \
  "$exitstub_writer_region" >/dev/null
if grep -F 'emit_asmlabel_addr(lj_vm_exit_handler)' \
     "$exitstub_writer_region" >/dev/null; then
  echo "ARM64 shared exit fallback regained placement-dependent VM branching" >&2
  exit 1
fi

awk '/^static void asm_exitstub_setup\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' \
  "$root/src/lj_asm_arm64.h" >"$exitstub_setup_region"
test -s "$exitstub_setup_region"
grep -F 'if (need >= 0x10000u)' "$exitstub_setup_region" >/dev/null
grep -F 'lj_mcode_limiterr(as->J, (size_t)(need + 4*MCLIM_REDZONE));' \
  "$exitstub_setup_region" >/dev/null
limit_line=$(grep -nF 'if (need >= 0x10000u)' \
  "$exitstub_setup_region" | sed -n '1p' | cut -d: -f1)
alloc_line=$(grep -nF 'as->T->exittab = lj_mem_newvec' \
  "$exitstub_setup_region" | sed -n '1p' | cut -d: -f1)
test "$limit_line" -lt "$alloc_line"

awk '/^static LJ_NORET LJ_NOINLINE void asm_mclimit\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_asm.c" >"$asm_mclimit_region"
test -s "$asm_mclimit_region"
grep -F 'as->mctoporig - as->mcp + 4*MCLIM_REDZONE' \
  "$asm_mclimit_region" >/dev/null

awk '/^void lj_asm_patchexit\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' \
  "$root/src/lj_asm_arm64.h" >"$patchexit_region"
test -s "$patchexit_region"
grep -F 'trace_exittarget_arm64_rel(J2G(J), T, exitno, target);' \
  "$patchexit_region" >/dev/null
if grep -E 'lj_mcode_|asm_mcode_fixup|emit_|mem(cpy|move|set)|__clear_cache|sys_icache_invalidate|pthread_jit_write_protect|mprotect|\[[^]]+\][[:space:]]*=|\*[[:space:]]*[[:alnum:]_]+[[:space:]]*=' \
     "$patchexit_region" >/dev/null; then
  echo "ARM64 exit retargeting mutates or synchronizes executable mcode" >&2
  exit 1
fi
grep -F 'ptrauth_auth_data(ptrauth_nop_cast(char *, target)' \
  "$root/src/lj_emit_arm64.h" >/dev/null

# Exercise the production table on a live certified root. The fixture checks
# heap ownership, default XPOLL/terminal exits, immutable gate bytes, isolated
# slot retargeting, reset, and the actual branch to a BTI-J/SIGTRAP landing.
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  -c "$root/tests/t-arm64-jit-exittab-trap.S" -o "$live_trap_object"
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-exittab.c" "$live_trap_object" "$archive" \
  -lm -pthread -o "$live_fixture"
"$live_fixture" supervise

# Run the generic trace-body retirement fixture under the actual ARM64 exit-slot
# policy. Its ARM64-only case checks PAC-stripped slot scans and exact nsnap+1
# teardown for a synthetic side-shaped body while side recording stays closed.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-jit-trace-retire.c" "$archive" -lm -pthread \
  -o "$trace_retire_fixture"
"$trace_retire_fixture"

# Rebuild the same single-slice contract for arm64e/BTI. This proves that LOOP
# entry is authenticated, each exit gate uses BRAA x30,x22, the shared K64
# fallback uses BLRAAZ, and all indirect landing pads have the right BTI kind.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
otool -hv "$root/src/lj_vm.o" | grep -E 'ARM64[[:space:]]+E' >/dev/null
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -dM -E -include lj_arch.h -x c /dev/null >"$pauth_macros"
for setting in \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_JIT_EXIT_TARGET_SLOTS 1' \
  'LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED 1'; do
  grep -F "#define $setting" "$pauth_macros" >/dev/null || {
    echo "ARM64e exit gate mismatch: $setting" >&2
    exit 1
  }
done
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" "$root/tests/t-arm64-jit-exit.c" \
  "$archive" -lm -pthread -o "$pauth_fixture"
"$pauth_fixture" "$pauth_words"
test "$(wc -c <"$pauth_words" | tr -d '[:space:]')" = 144
"$cc" -arch arm64e -mmacosx-version-min="$minver" -x assembler \
  -c /dev/null -o "$pauth_empty"
ld -r -arch arm64e -o "$pauth_stub_object" "$pauth_empty" \
  -sectcreate __TEXT __text "$pauth_words"
otool -tvV "$pauth_stub_object" >"$pauth_stub_disasm"
extract_exit_code "$pauth_stub_disasm" "$pauth_stub_code_disasm"
test "$(grep -Ec 'bti[[:space:]]+j$' "$pauth_stub_code_disasm")" = 1
test "$(grep -Ec 'ldr[[:space:]]+x30, \[x22(, #0x[[:xdigit:]]+)?\]' \
  "$pauth_stub_code_disasm")" = 1
test "$(grep -Ec 'blraaz[[:space:]]+x30$' \
  "$pauth_stub_code_disasm")" = 1
test "$(grep -Ec 'mov[[:space:]]+w0, #0x1234$' \
  "$pauth_stub_code_disasm")" = 1
test "$(grep -Ec 'mov[[:space:]]+w30, #0x[[:xdigit:]]+$' \
  "$pauth_stub_code_disasm")" = 4
test "$(grep -Ec 'str[[:space:]]+x30, \[sp\]$' \
  "$pauth_stub_code_disasm")" = 4
test "$(grep -Ec 'ldar[[:space:]]+x30, \[x30\]$' \
  "$pauth_stub_code_disasm")" = 4
test "$(grep -Ec 'braa[[:space:]]+x30, x22$' \
  "$pauth_stub_code_disasm")" = 4
test "$(grep -Ec 'nop$' "$pauth_stub_code_disasm")" = 4
test "$(grep -Ec 'br[[:space:]]+x30$|blr[[:space:]]+x30$|[[:space:]]bl[[:space:]]' \
  "$pauth_stub_code_disasm" || true)" = 0
otool -tvV "$root/src/lj_vm.o" >"$pauth_vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' \
  "$pauth_vm_disasm" >"$pauth_jloop_region"
awk '/^_lj_vm_exit_handler:/ { copy=1 }
     copy { print }
     copy && /[[:space:]]bl[[:space:]]/ { exit }' \
  "$pauth_vm_disasm" >"$pauth_handler_ingress_region"
test -s "$pauth_jloop_region" && test -s "$pauth_handler_ingress_region"
grep -E 'sub[[:space:]]+sp, sp, #0x10' \
  "$pauth_jloop_region" >/dev/null
test "$(grep -Ec 'braa[[:space:]]+x1, x0$' \
  "$pauth_jloop_region")" = 1
pauth_sub=$(grep -nE 'sub[[:space:]]+sp, sp, #0x10' \
  "$pauth_jloop_region" | sed -n '1p' | cut -d: -f1)
pauth_branch=$(grep -nE 'braa[[:space:]]+x1, x0$' \
  "$pauth_jloop_region" | sed -n '1p' | cut -d: -f1)
test "$pauth_sub" -lt "$pauth_branch"
grep -E 'add[[:space:]]+x2, sp, #0x200' \
  "$pauth_handler_ingress_region" >/dev/null
grep -E 'ldr[[:space:]]+w0, \[x2\]' \
  "$pauth_handler_ingress_region" >/dev/null
if grep -E 'ldr[[:space:]]+x0, \[sp, #0x1f8\]|sub[[:space:]]+x0,|lsr[[:space:]]+w0,' \
     "$pauth_handler_ingress_region" >/dev/null; then
  echo "ARM64e exit handler still derives exit number from the saved LR" >&2
  exit 1
fi
awk '/^_lj_vm_exit_handler:/ { getline; exit($0 ~ /bti[[:space:]]+c/ ? 0 : 1) }' \
  "$pauth_vm_disasm"
awk '/^_lj_vm_exit_interp:/ { getline; exit($0 ~ /bti[[:space:]]+j/ ? 0 : 1) }' \
  "$pauth_vm_disasm"
otool -tvV "$root/src/lj_asm.o" | grep -E '[[:space:]]autiz?a[[:space:]]' >/dev/null

# Repeat the live contract as an ARM64e process. In addition to the successful
# globally signed target, the supervisor requires raw, zero-context,
# trace-context, and wrong-global publications to fault before the BRK landing.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" \
  -c "$root/tests/t-arm64-jit-exittab-trap.S" \
  -o "$pauth_live_trap_object"
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" "$root/tests/t-arm64-jit-exittab.c" \
  "$pauth_live_trap_object" "$archive" -lm -pthread \
  -o "$pauth_live_fixture"
"$pauth_live_fixture" supervise
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" "$root/tests/t-jit-trace-retire.c" \
  "$archive" -lm -pthread -o "$pauth_trace_retire_fixture"
"$pauth_trace_retire_fixture"

# Leave the isolated checkout in its ordinary native experimental mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' XCFLAGS="$xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$xcflags"
restore_needed=0

echo "arm64_jit_exit_contract OK: immutable arm64/arm64e heap-target gates and shared K64 fallback verified"
