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
handler_region=$tmpdir/vm-exit-handler.txt
interp_region=$tmpdir/vm-exit-interp.txt
trace_region=$tmpdir/trace-exit.txt
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
pauth_fixture=$tmpdir/t-arm64-jit-exit-arm64e
pauth_words=$tmpdir/exit-stubs-arm64e.bin
pauth_empty=$tmpdir/empty-arm64e.o
pauth_stub_object=$tmpdir/exit-stubs-arm64e.o
pauth_stub_disasm=$tmpdir/exit-stubs-arm64e.disasm
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm

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
awk '/^_lj_vm_exit_handler:/ { copy=1 }
     copy { print }
     copy && /^_lj_vm_exit_interp:/ { exit }' "$vm_disasm" >"$handler_region"
awk '/^_lj_vm_exit_interp:/ { copy=1 }
     copy { print }
     copy && /^_lj_vm_modi:/ { exit }' "$vm_disasm" >"$interp_region"
test -s "$handler_region" && test -s "$interp_region"
grep -E 'add[[:space:]]+x14, x25' "$handler_region" >/dev/null
grep -E 'ldar[[:space:]]+x19, \[x14\]' "$handler_region" >/dev/null
if grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$handler_region" >/dev/null; then
  echo "ARM64 exit handler clears the TG trace lease before C restore" >&2
  exit 1
fi
test "$(grep -Ec 'stlr[[:space:]]+xzr, \[x14\]' "$interp_region")" = 2
test "$(grep -Ec 'ldar[[:space:]]+w8, \[x14\]' "$interp_region")" = 2
test "$(grep -Ec 'ldar[[:space:]]+w9, \[x14\]' "$interp_region")" = 2

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
grep -F '#if LJ_TARGET_X64 || LJ_TARGET_ARM64' "$root/src/lj_err.c" >/dev/null
grep -F 'lj_tg_jit_exitcode_rel(G2TG(g), errcode);' \
  "$root/src/lj_err.c" >/dev/null
test "$(grep -Fc 'emit_asmfunc_addr((ASMFunction)lj_vm_' \
  "$root/src/lj_asm_arm64.h")" = 3
test "$(grep -Fc 'emit_asmfunc_addr((ASMFunction)lj_vm_exit_interp)' \
  "$root/src/lj_asm_arm64.h")" = 2
grep -F 'ptrauth_auth_data((char *)target' \
  "$root/src/lj_emit_arm64.h" >/dev/null

# Rebuild the same contract for the arm64e/BTI slice. This proves that direct
# target arithmetic compiles through authenticated code-address normalization,
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
awk '/^_lj_vm_exit_handler:/ { getline; exit($0 ~ /bti[[:space:]]+c/ ? 0 : 1) }' \
  "$pauth_vm_disasm"
awk '/^_lj_vm_exit_interp:/ { getline; exit($0 ~ /bti[[:space:]]+j/ ? 0 : 1) }' \
  "$pauth_vm_disasm"
otool -tvV "$root/src/lj_asm.o" | grep -E '[[:space:]]autiz?a[[:space:]]' >/dev/null

# Leave the isolated checkout in its ordinary native experimental mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

echo "arm64_jit_exit_contract OK: stubs, TG lease, VM quiescence and arm64e/BTI verified"
