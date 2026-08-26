#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_root_entry_contract SKIP: requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
cleanup() {
  if test "$lock_held" = 1; then
    rm -f "$lock_dir/owner"
    rmdir "$lock_dir" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}

while ! mkdir "$lock_dir" 2>/dev/null; do sleep 0.2; done
lock_held=1
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-root-entry.XXXXXX")
trap cleanup EXIT HUP INT TERM
printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true

jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
vm_object=$root/src/lj_vm.o
fixture_obj=$tmpdir/t-arm64-jit-root-entry.o
fixture=$tmpdir/t-arm64-jit-root-entry
disasm=$tmpdir/fixture.disasm
abi_region=$tmpdir/abi-region.txt
helper_region=$tmpdir/helper-region.txt
pending_region=$tmpdir/pending-region.txt
vm_disasm=$tmpdir/vm.disasm
vm_reloc=$tmpdir/vm.reloc
jloop_region=$tmpdir/vm-jloop.txt
jfuncf_region=$tmpdir/vm-jfuncf.txt
jfuncv_region=$tmpdir/vm-jfuncv.txt
pauth_fixture=$tmpdir/t-arm64-jit-root-entry-arm64e
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" XCFLAGS="$xcflags" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

test "$(lipo -archs "$archive")" = arm64
nm "$archive" | grep ' T _lj_trace_enter_root$' >/dev/null
nm "$vm_object" | grep ' U _lj_trace_enter_root$' >/dev/null

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/tests/t-arm64-jit-root-entry.c" -o "$fixture_obj"
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-root-entry.c" "$archive" -lm -pthread -o "$fixture"
"$fixture"

# Prove the checked-in caller consumes the 16-byte non-HFA result in x0/x1:
# x6 (the seventh argument) is preserved, the helper has a direct BR26 call,
# x1 is stored as target, and x0 is returned without a hidden x8 sret pointer.
otool -tvV "$fixture_obj" >"$disasm"
awk '/^_lj_test_root_entry_abi_probe:/ { copy=1 }
     copy { print }
     copy && /^_main:/ { exit }' "$disasm" >"$abi_region"
grep -E 'mov[[:space:]]+x19, x6' "$abi_region" >/dev/null
grep -E 'str[[:space:]]+x1, \[x19\]' "$abi_region" >/dev/null
grep -E 'ret$' "$abi_region" >/dev/null
if grep -E '(^|[[:space:],])x8([[:space:],]|$)' "$abi_region" >/dev/null; then
  echo "root-entry ABI probe unexpectedly uses hidden-result register x8" >&2
  exit 1
fi
otool -rv "$fixture_obj" | \
  grep '^00000010 .* BR26 .* _lj_trace_enter_root$' >/dev/null

# The generated VM has two real AAPCS64 call sites, one each for JLOOP and
# JFUNCF. JFUNCV remains the explicit trap until its frame transform is ported.
otool -rv "$vm_object" >"$vm_reloc"
test "$(grep -c 'BR26.*_lj_trace_enter_root$' "$vm_reloc")" = 2
otool -tvV "$vm_object" >"$vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' "$vm_disasm" >"$jloop_region"
awk '/^_lj_BC_JFUNCF:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_FUNCV:/ { exit }' "$vm_disasm" >"$jfuncf_region"
awk '/^_lj_BC_JFUNCV:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_FUNCC:/ { exit }' "$vm_disasm" >"$jfuncv_region"
test -s "$jloop_region" && test -s "$jfuncf_region" && test -s "$jfuncv_region"
for region in "$jloop_region" "$jfuncf_region"; do
  grep -E 'mov[[:space:]]+x4, x19' "$region" >/dev/null
  grep -E 'mov[[:space:]]+w5, #0x' "$region" >/dev/null
  grep -E 'ldar[[:space:]]+w16, \[x14\]' "$region" >/dev/null
  grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$region" >/dev/null
  grep -E 'br[[:space:]]+x1' "$region" >/dev/null
done
grep -E 'mov[[:space:]]+w2, w28' "$jloop_region" >/dev/null
grep -E 'add[[:space:]]+x8, x21, x28, lsl #2' "$jloop_region" >/dev/null
grep -E 'sub[[:space:]]+x21, x8, #0x20, lsl #12' \
  "$jloop_region" >/dev/null
if grep -E 'ubfx[[:space:]]+x28' "$jfuncf_region" >/dev/null; then
  echo "JFUNCF rejection clobbers the preserved actual-nargs register" >&2
  exit 1
fi
grep -E 'brk[[:space:]]+#0' "$jfuncv_region" >/dev/null

jloop_source=$tmpdir/vm-jloop.dasc
jfuncf_source=$tmpdir/vm-jfuncf.dasc
for_source=$tmpdir/vm-for.dasc
awk '/case BC_FORL:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /case BC_ITERL:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$for_source"
awk '/case BC_JLOOP:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /case BC_JMP:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$jloop_source"
awk '/case BC_JFUNCF:/ { seen++; if (seen == 2) copy=1 }
     copy { print }
     copy && /case BC_JFUNCV:/ { exit }' \
  "$root/src/vm_arm64.dasc" >"$jfuncf_source"
for source in "$jloop_source" "$jfuncf_source"; do
  grep -F 'add CARG1, GL, #GG_G2J_HI' "$source" >/dev/null
  grep -F 'sub CARG2, PC, #4' "$source" >/dev/null
  grep -F 'decode_RD CARG3, INS' "$source" >/dev/null
  grep -F 'mov CARG4, L' "$source" >/dev/null
  grep -F 'mov CARG5, BASE' "$source" >/dev/null
  grep -F 'bl extern lj_trace_enter_root' "$source" >/dev/null
  grep -F 'br_trace_auth CARG2, CRET1' "$source" >/dev/null
done
grep -F 'mov CARG6w, #BC_JLOOP' "$jloop_source" >/dev/null
grep -F 'mov CARG3w, RCw' "$jloop_source" >/dev/null
for required in '#BC_JFORL' '#BC_JITERL' '#BC_FORL' '#BC_ITERL' \
  'add TMP0, PC, RC, lsl #2' 'arm64_vm_poll_acq TMP0w, TMP1w'; do
  grep -F "$required" "$jloop_source" >/dev/null
done
test "$(grep -Fc 'cmp TMP0w, #BC_JLOOP' "$jloop_source")" -ge 3
test "$(grep -Fc 'mov PC, RC' "$for_source")" = 2
test "$(grep -Fc 'ldrh RCw, [RC, #-4+OFS_RD]' "$for_source")" = 1
test "$(grep -Fc 'ldrh RCw, [PC, #-4+OFS_RD]' "$for_source")" = 1
grep -F 'mov CARG6w, #BC_JFUNCF' "$jfuncf_source" >/dev/null
grep -F 'Preserve callee-save RC' "$jfuncf_source" >/dev/null

# The aggregate remains a conservative out-of-tree signal. Every in-tree
# fixture names the boundary whose behavior it actually relies on.
arch_predicates=$tmpdir/arm64-jit-predicates.txt
awk '/#ifndef LJ_ARM64_JIT_FAIL_CLOSED/ { copy=1 }
     copy { print }
     copy && /^#endif/ { exit }' "$root/src/lj_arch.h" >"$arch_predicates"
grep -F 'LJ_ARM64_JIT_RECORDER_ADMISSION_FAIL_CLOSED ||' \
  "$arch_predicates" >/dev/null
grep -F 'LJ_ARM64_JIT_NATIVE_ENTRY_FAIL_CLOSED)' "$arch_predicates" >/dev/null
if grep -R 'LJ_ARM64_JIT_FAIL_CLOSED' \
     "$root/tests/t-arm64-jit-emitter.c" \
     "$root/tests/t-arm64-jit-exit.c" \
     "$root/tests/t-jit-recorder-safepoint.c" \
     "$root/tests/t-vm-safepoint.c" >/dev/null; then
  echo "ARM64 fixtures still use the ambiguous aggregate predicate" >&2
  exit 1
fi

root_fixture_source=$root/tests/t-arm64-jit-root-entry.c
for required in '__arm64_root_jforl' '__arm64_root_jfori' \
  'lua_tointeger(L, -1) == 1234' \
  'lua_pushnumber(L, 4.5)' \
  'lj_trace_test_root_entry_retry_restore(loop.pc, loop.original)' \
  'lj_trace_test_root_entry_startins_calls() == 1' \
  'ROOT_ENTRY_POLL_AFTER_METADATA' \
  'ROOT_ENTRY_REQMASK_AFTER_METADATA' \
  'ROOT_ENTRY_PROFILE_AFTER_METADATA'; do
  grep -F "$required" "$root_fixture_source" >/dev/null
done

# Keep the semantic ordering reviewable independently of compiler scheduling.
awk '/^lj_trace_enter_root\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$helper_region"
test -s "$helper_region"
line_of() { grep -n "$1" "$helper_region" | sed -n "${2:-1}p" | cut -d: -f1; }
gate1=$(line_of 'lj_gc2_jit_entry_open(g)' 1)
publish=$(line_of 'lj_tg_store_jit_base(tg, base)' 1)
fence=$(line_of 'la_fence_seq()' 1)
gate2=$(line_of 'lj_gc2_jit_entry_open(g)' 2)
slot1=$(line_of 'trace_root_entry_slot_acq(J, traceno' 1)
mcode=$(line_of 'trace_mcode_acq(T)' 1)
slot2=$(line_of 'trace_root_entry_slot_acq(J, traceno' 2)
pending1=$(line_of 'trace_root_entry_request_pending(tg)' 1)
pending2=$(line_of 'trace_root_entry_request_pending(tg)' 2)
pending3=$(line_of 'trace_root_entry_request_pending(tg)' 3)
postmetadata=$(line_of 'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA' 1)
native_gate=$(line_of 'LJ_ARM64_JIT_NATIVE_ENTRY_FAIL_CLOSED' 1)
tmpbuf=$(line_of 'setsbufL(&tg->tmpbuf, L)' 1)
cleanup_line=$(line_of 'lj_tg_store_jit_base(tg, NULL)' 1)
test "$pending1" -lt "$gate1" && test "$gate1" -lt "$publish" &&
test "$publish" -lt "$fence" && test "$fence" -lt "$gate2" &&
test "$gate2" -lt "$pending2" && test "$pending2" -lt "$slot1" &&
test "$slot1" -lt "$mcode" && test "$mcode" -lt "$slot2" &&
test "$slot2" -lt "$postmetadata" && test "$postmetadata" -lt "$pending3" &&
test "$pending3" -lt "$native_gate" &&
test "$native_gate" -lt "$tmpbuf" && test "$tmpbuf" -lt "$cleanup_line"
test "$(grep -c 'lj_tg_store_jit_base(tg, NULL)' "$helper_region")" = 1
for required in trace_runnable_acq trace_root_acq trace_startpc_acq \
  trace_startins_acq trace_szmcode_acq trace_mcauth_acq lj_ptr_strip; do
  grep "$required" "$helper_region" >/dev/null
done
awk '/^static LJ_AINLINE int trace_root_entry_request_pending/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$pending_region"
for required in lj_tg_poll_acq lj_tg_reqmask_acq \
  lj_tg_profile_request_acq; do
  grep "$required" "$pending_region" >/dev/null
done
if grep -E 'lj_gc2_smr|lj_mem_|lj_alloc|lj_err_|lj_vmevent|lj_safepoint' \
     "$helper_region" >/dev/null; then
  echo "root-entry helper acquired a forbidden blocking/allocating surface" >&2
  exit 1
fi

env LUA_PATH="$root/src/?.lua;$root/src/jit/?.lua;;" "$root/src/luajit" -e '
  local util = require("jit.util")
  jit.flush(); jit.on(); jit.opt.start("hotloop=1")
  local n = 0
  for r = 1, 64 do for i = 1, 256 do n = n + i end end
  assert(n == 64 * 256 * 257 / 2)
  assert(util.traceinfo(1) == nil, "root-entry tranche enabled recording")
'

# Rebuild and execute the rejection fixture as arm64e with BTI enabled. The
# success instruction must authenticate x1 with the exact x0 GCtrace modifier;
# BRAAZ would silently use the wrong discriminator.
env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
otool -hv "$vm_object" | grep -E 'ARM64[[:space:]]+E' >/dev/null
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_xcflags -I"$root/src" "$root/tests/t-arm64-jit-root-entry.c" \
  "$archive" -lm -pthread -o "$pauth_fixture"
"$pauth_fixture"
otool -tvV "$vm_object" >"$pauth_vm_disasm"
test "$(grep -Ec 'braa[[:space:]]+x1, x0' "$pauth_vm_disasm")" = 2
if grep -E 'braaz[[:space:]]+x1$' "$pauth_vm_disasm" >/dev/null; then
  echo "ARM64e root entry authenticates without the GCtrace modifier" >&2
  exit 1
fi

# Leave the isolated checkout in its ordinary native experimental mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

echo "arm64_jit_root_entry_contract OK: VM ABI, rejection, request races, ARM64e modifier and fail-closed surfaces verified"
