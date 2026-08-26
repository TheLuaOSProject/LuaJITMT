#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_exit_contract SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-jit-exit.XXXXXX")
cleanup() {
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM
while ! mkdir "$lock_dir" 2>/dev/null; do sleep 0.2; done
lock_held=1
printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLJ_ARM64_EXIT_TEST_HELPERS'
archive=$root/src/libluajit.a
fixture=$tmpdir/t-arm64-jit-exit
stub_words=$tmpdir/exit-stubs.bin
empty_object=$tmpdir/empty.o
stub_object=$tmpdir/exit-stubs.o
stub_disasm=$tmpdir/exit-stubs.disasm
vm_disasm=$tmpdir/vm.disasm
jloop_region=$tmpdir/vm-jloop.txt
handler_region=$tmpdir/vm-exit-handler.txt
interp_region=$tmpdir/vm-exit-interp.txt
stale_dispatch_region=$tmpdir/vm-exit-stale-dispatch.txt
trace_region=$tmpdir/trace-exit.txt
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
pauth_fixture=$tmpdir/t-arm64-jit-exit-arm64e
pauth_words=$tmpdir/exit-stubs-arm64e.bin
pauth_empty=$tmpdir/empty-arm64e.o
pauth_stub_object=$tmpdir/exit-stubs-arm64e.o
pauth_stub_disasm=$tmpdir/exit-stubs-arm64e.disasm
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm
pauth_jloop_region=$tmpdir/vm-arm64e-jloop.txt

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

env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_asm_arm64_exitstub_test$' >/dev/null

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-exit.c" "$archive" -lm -pthread -o "$fixture"
"$fixture" "$stub_words"

"$cc" -arch arm64 -mmacosx-version-min="$minver" -x assembler \
  -c /dev/null -o "$empty_object"
ld -r -arch arm64 -o "$stub_object" "$empty_object" \
  -sectcreate __TEXT __text "$stub_words"
otool -tvV "$stub_object" >"$stub_disasm"
test "$(grep -Ec 'str[[:space:]]+x30, \[sp\]' "$stub_disasm")" = 2
test "$(grep -Ec 'mov[[:space:]]+w0, #0x1234' "$stub_disasm")" = 2
test "$(grep -Ec 'blr[[:space:]]+x30' "$stub_disasm")" = 1
test "$(grep -Ec '[[:space:]]bl[[:space:]]' "$stub_disasm")" -ge 9

otool -tvV "$root/src/lj_vm.o" >"$vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' "$vm_disasm" >"$jloop_region"
awk '/^_lj_vm_exit_handler:/ { copy=1 }
     copy { print }
     copy && /^_lj_vm_exit_interp:/ { exit }' "$vm_disasm" >"$handler_region"
awk '/^_lj_vm_exit_interp:/ { copy=1 }
     copy { print }
     copy && /^_lj_vm_modi:/ { exit }' "$vm_disasm" >"$interp_region"
test -s "$jloop_region" && test -s "$handler_region" && \
  test -s "$interp_region"
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jloop_region" >/dev/null
test "$(grep -Ec 'br[[:space:]]+x1$' "$jloop_region")" = 1
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
interp_source=$tmpdir/vm-exit-interp.dasc
awk '/[|]->vm_exit_handler:/ { copy=1 }
     copy { print }
     copy && /[|]->vm_exit_interp:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$handler_source"
awk '/[|]->vm_exit_interp:/ { copy=1 }
     copy { print }
     copy && /[|]->vm_modi:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$interp_source"
grep -F 'ld_tg_jit_base BASE' "$handler_source" >/dev/null
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
test "$(grep -Fc 'emit_asmlabel_addr(lj_vm_' \
  "$root/src/lj_asm_arm64.h")" = 3
test "$(grep -Fc 'emit_asmlabel_addr(lj_vm_exit_interp)' \
  "$root/src/lj_asm_arm64.h")" = 2
grep -F 'ptrauth_auth_data(ptrauth_nop_cast(char *, target)' \
  "$root/src/lj_emit_arm64.h" >/dev/null

# Rebuild the same contract for the arm64e/BTI slice. This proves that direct
# VM-label arithmetic stays raw, genuine function pointers authenticate,
# indirect stubs use BLRAAZ, and the two VM landing pads have the right BTI kind.
env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
otool -hv "$root/src/lj_vm.o" | grep -E 'ARM64[[:space:]]+E' >/dev/null
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" "$root/tests/t-arm64-jit-exit.c" \
  "$archive" -lm -pthread -o "$pauth_fixture"
"$pauth_fixture" "$pauth_words"
"$cc" -arch arm64e -mmacosx-version-min="$minver" -x assembler \
  -c /dev/null -o "$pauth_empty"
ld -r -arch arm64e -o "$pauth_stub_object" "$pauth_empty" \
  -sectcreate __TEXT __text "$pauth_words"
otool -tvV "$pauth_stub_object" >"$pauth_stub_disasm"
grep -E 'blraaz[[:space:]]+x30' "$pauth_stub_disasm" >/dev/null
otool -tvV "$root/src/lj_vm.o" >"$pauth_vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' \
  "$pauth_vm_disasm" >"$pauth_jloop_region"
test -s "$pauth_jloop_region"
if grep -E '(braa(z)?[[:space:]]+x1([,[:space:]]|$)|br[[:space:]]+x1$)' \
     "$pauth_jloop_region" >/dev/null; then
  echo "arm64e JLOOP unexpectedly contains a native helper-target entry" >&2
  exit 1
fi
if grep -E 'sub[[:space:]]+sp, sp, #0x10' \
     "$pauth_jloop_region" >/dev/null; then
  echo "arm64e JLOOP unexpectedly reserves a native trace frame" >&2
  exit 1
fi
grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$pauth_jloop_region" >/dev/null
awk '/^_lj_vm_exit_handler:/ { getline; exit($0 ~ /bti[[:space:]]+c/ ? 0 : 1) }' \
  "$pauth_vm_disasm"
awk '/^_lj_vm_exit_interp:/ { getline; exit($0 ~ /bti[[:space:]]+j/ ? 0 : 1) }' \
  "$pauth_vm_disasm"
otool -tvV "$root/src/lj_asm.o" | grep -E '[[:space:]]autiz?a[[:space:]]' >/dev/null

# Leave the isolated checkout in its ordinary native experimental mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

echo "arm64_jit_exit_contract OK: arm64 LOOP-open and arm64e LOOP-closed exit policies verified"
