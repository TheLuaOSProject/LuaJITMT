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
ordinary_macros=$tmpdir/macros-arm64.txt
helper_region=$tmpdir/helper-region.txt
pending_region=$tmpdir/pending-region.txt
source_gate_region=$tmpdir/source-gate-region.txt
loop_view_region=$tmpdir/loop-view-region.txt
vm_disasm=$tmpdir/vm.disasm
vm_reloc=$tmpdir/vm.reloc
jloop_region=$tmpdir/vm-jloop.txt
jfuncf_region=$tmpdir/vm-jfuncf.txt
jfuncv_region=$tmpdir/vm-jfuncv.txt
pauth_macros=$tmpdir/macros-arm64e.txt
pauth_vm_disasm=$tmpdir/vm-arm64e.disasm
pauth_jloop_region=$tmpdir/vm-arm64e-jloop.txt

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

# The generated VM has two real AAPCS64 helper call sites. Only strict JLOOP
# may continue through the 16-byte SPS_FIXED reservation to native code;
# JFUNCF remains closed at the helper's granular source gate.
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
done
grep -E 'sub[[:space:]]+sp, sp, #0x10' "$jloop_region" >/dev/null
grep -E 'br[[:space:]]+x1$' "$jloop_region" >/dev/null
if grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$jloop_region" >/dev/null; then
  echo "open JLOOP unexpectedly clears its successful helper lease" >&2
  exit 1
fi
grep -E 'ldar[[:space:]]+w16, \[x14\]' "$jfuncf_region" >/dev/null
grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$jfuncf_region" >/dev/null
if grep -E 'br[[:space:]]+x1$' "$jfuncf_region" >/dev/null; then
  echo "closed JFUNCF unexpectedly branches to the helper target" >&2
  exit 1
fi
jloop_call=$(grep -nE 'bl[[:space:]]+0x' "$jloop_region" | sed -n '1p' | cut -d: -f1)
jloop_sub=$(grep -nE 'sub[[:space:]]+sp, sp, #0x10' \
  "$jloop_region" | sed -n '1p' | cut -d: -f1)
jloop_branch=$(grep -nE 'br[[:space:]]+x1$' "$jloop_region" | \
  sed -n '1p' | cut -d: -f1)
test -n "$jloop_call" && test "$jloop_call" -lt "$jloop_sub" &&
test "$jloop_sub" -lt "$jloop_branch"
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
done
grep -F 'mov CARG6w, #BC_JLOOP' "$jloop_source" >/dev/null
grep -F 'mov CARG3w, RCw' "$jloop_source" >/dev/null
grep -F '#if LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED' \
  "$jloop_source" >/dev/null
grep -F 'sub sp, sp, #16' "$jloop_source" >/dev/null
grep -F 'br_trace_auth CARG2, CRET1' "$jloop_source" >/dev/null
jloop_source_call=$(grep -n 'bl extern lj_trace_enter_root' "$jloop_source" |
  sed -n '1p' | cut -d: -f1)
jloop_source_sub=$(grep -n 'sub sp, sp, #16' "$jloop_source" |
  sed -n '1p' | cut -d: -f1)
jloop_source_branch=$(grep -n 'br_trace_auth CARG2, CRET1' "$jloop_source" |
  sed -n '1p' | cut -d: -f1)
test "$jloop_source_call" -lt "$jloop_source_sub" &&
test "$jloop_source_sub" -lt "$jloop_source_branch"
for required in '#BC_JFORL' '#BC_JITERL' '#BC_FORL' '#BC_ITERL' \
  'add TMP0, PC, RC, lsl #2' 'arm64_vm_poll_acq TMP0w, TMP1w'; do
  grep -F "$required" "$jloop_source" >/dev/null
done
test "$(grep -Fc 'cmp TMP0w, #BC_JLOOP' "$jloop_source")" -ge 3
test "$(grep -Fc 'mov PC, RC' "$for_source")" = 2
test "$(grep -Fc 'ldrh RCw, [RC, #-4+OFS_RD]' "$for_source")" = 1
test "$(grep -Fc 'ldrh RCw, [PC, #-4+OFS_RD]' "$for_source")" = 1
grep -F 'mov CARG6w, #BC_JFUNCF' "$jfuncf_source" >/dev/null
grep -F '#if LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' \
  "$jfuncf_source" >/dev/null
grep -F 'clear_tg_jit_base' "$jfuncf_source" >/dev/null
grep -F 'Preserve callee-save RC' "$jfuncf_source" >/dev/null
grep -F '#if LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' \
  "$root/src/lj_trace.c" >/dev/null

# The aggregate remains a conservative out-of-tree signal. This fixture names
# the exact open and closed recorder/entry surfaces it relies on. Inspect the
# evaluated ordinary-ARM64 values, rather than matching one conditional arm in
# the source header.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -dM -E -include lj_arch.h -x c /dev/null >"$ordinary_macros"
grep -E '^#define LJ_ABI_PAUTH[[:space:]]+0$' \
  "$ordinary_macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED[[:space:]]+0$' \
  "$ordinary_macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
  "$ordinary_macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
  "$ordinary_macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED[[:space:]]+0$' \
  "$ordinary_macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED[[:space:]]+1$' \
  "$ordinary_macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED[[:space:]]+1$' \
  "$ordinary_macros" >/dev/null
if grep -R 'LJ_ARM64_JIT_FAIL_CLOSED' \
     "$root/tests/t-arm64-jit-root-entry.c" \
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
  'LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED' \
  'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED' \
  'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' \
  'trace_startpt_rel(&J->cur, pt);' \
  'trace_link_rel(&J->cur, 1);' \
  'J->cur.linktype = LJ_TRLINK_LOOP;' \
  'J->cur.spadjust = 0;' \
  'J->cur.mcloop = (MSize)sizeof(MCode);' \
  'J->cur.unused1 = TRACE_ARM64_INT_LOOP_ADMITTED;' \
  'J->cur.ir = fixture->ir;' \
  'J->cur.snap = fixture->snap;' \
  'expect_metadata_success(L, metadata_loop.pc, &metadata);' \
  'lj_tg_store_jit_base(L->tg_hint, NULL);' \
  'T->unused1 &= (uint8_t)~TRACE_ARM64_INT_LOOP_ADMITTED;' \
  'T->retire_epoch = 1;' \
  'T->unused1 |= TRACE_ENTRY_INVALIDATED;' \
  'T->startins = BCINS_AJ(BC_LOOP, bc_a(loop->original), 0);' \
  'trace_link_rel(T, 2);' \
  'trace_traceno_rel(T, 2);' \
  'T->mcloop = 0;' \
  'L->base = stack + LJ_FR2;' \
  'L->base = maxstack-1;' \
  'metadata_pt->framesize > (MSize)(maxstack-L->base)' \
  'lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);' \
  'lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_C);' \
  'lj_tg_vmstate_store_rel(tg, saved_vmstate);' \
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
source_gate=$(line_of '!trace_root_entry_source_admitted(sourceop)' 1)
frame_bound=$(line_of 'basep - stackp <' 1)
interp_state=$(line_of \
  'lj_tg_vmstate_load_acq(tg) != (int32_t)~LJ_VMST_INTERP' 1)
current_func=$(line_of 'fn = curr_func(L)' 1)
prototype=$(line_of 'pt = funcproto(fn)' 1)
frame_extent=$(line_of 'pt->framesize >' 1)
gate1=$(line_of 'lj_gc2_jit_entry_open(g)' 1)
publish=$(line_of 'lj_tg_store_jit_base(tg, base)' 1)
fence=$(line_of 'la_fence_seq()' 1)
gate2=$(line_of 'lj_gc2_jit_entry_open(g)' 2)
slot1=$(line_of 'trace_root_entry_slot_acq(J, traceno' 1)
view1=$(line_of 'trace_root_entry_loop_view_acq(T, traceno' 1)
mcode=$(line_of 'mcode = view.mcode' 1)
slot2=$(line_of 'trace_root_entry_slot_acq(J, traceno' 2)
view2=$(line_of 'trace_root_entry_loop_view_acq(T2, traceno' 1)
view_equal=$(line_of 'trace_root_entry_loop_view_equal(&view, &view2)' 1)
pending1=$(line_of 'trace_root_entry_request_pending(tg)' 1)
pending2=$(line_of 'trace_root_entry_request_pending(tg)' 2)
pending3=$(line_of 'trace_root_entry_request_pending(tg)' 3)
postmetadata=$(line_of 'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA' 1)
tmpbuf=$(line_of 'setsbufL(&tg->tmpbuf, L)' 1)
cleanup_line=$(line_of 'lj_tg_store_jit_base(tg, NULL)' 1)
test "$source_gate" -lt "$frame_bound" &&
test "$frame_bound" -lt "$interp_state" &&
test "$interp_state" -lt "$pending1" &&
test "$pending1" -lt "$current_func" &&
test "$current_func" -lt "$prototype" &&
test "$prototype" -lt "$frame_extent" &&
test "$frame_extent" -lt "$gate1" &&
test "$gate1" -lt "$publish" &&
test "$publish" -lt "$fence" && test "$fence" -lt "$gate2" &&
test "$gate2" -lt "$pending2" && test "$pending2" -lt "$slot1" &&
test "$slot1" -lt "$view1" && test "$view1" -lt "$mcode" &&
test "$mcode" -lt "$slot2" && test "$slot2" -lt "$view2" &&
test "$view2" -lt "$view_equal" && test "$view_equal" -lt "$postmetadata" &&
test "$postmetadata" -lt "$pending3" && test "$pending3" -lt "$tmpbuf" &&
test "$tmpbuf" -lt "$cleanup_line"
test "$(grep -c 'lj_tg_store_jit_base(tg, NULL)' "$helper_region")" = 1
for required in trace_runnable_acq trace_startins_acq \
  trace_root_entry_loop_view_acq trace_root_entry_loop_view_equal \
  trace_mcauth_acq lj_ptr_strip; do
  grep "$required" "$helper_region" >/dev/null
done
awk '/^static LJ_AINLINE int trace_root_entry_source_admitted/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$source_gate_region"
grep -F 'LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED' \
  "$source_gate_region" >/dev/null
grep -F 'LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED' \
  "$source_gate_region" >/dev/null
awk '/^static int trace_root_entry_loop_view_acq/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$loop_view_region"
for required in trace_root_acq trace_link_acq trace_linktype_acq \
  trace_nextside_acq trace_nchild_acq trace_spadjust_acq trace_startptgco_acq \
  trace_startpc_acq trace_mcloop_acq trace_topslot_acq trace_ir_acq \
  trace_snap_acq trace_snapmap_acq trace_traceno_acq retire_epoch \
  TRACE_ENTRY_GATED TRACE_ARM64_INT_LOOP_ADMITTED; do
  grep "$required" "$loop_view_region" >/dev/null
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
  jit.flush(); jit.on(); jit.opt.start("hotloop=1", "hotexit=1", "maxtrace=2")
  local function f(n)
    local i, x = 0, 0
    while i < n do i = i + 1; x = x + i end
    return x
  end
  assert(f(20) == 210); assert(f(20) == 210); assert(f(20) == 210)
  assert(f(20) == 210); assert(f(20) == 210)
  assert(util.traceinfo(1), "open integer-loop root did not record")
  assert(util.traceinfo(2) == nil, "closed side/stitch surface published trace 2")
'

# Arm64e/BTI remains buildable, but both root recording and LOOP native entry
# are deliberately closed until authenticated trace symbols can be published
# safely. Do not compile the ordinary open-helper fixture under this ABI.
env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_xcflags"
otool -hv "$vm_object" | grep -E 'ARM64[[:space:]]+E' >/dev/null
# shellcheck disable=SC2086 # pauth_xcflags intentionally expands.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_xcflags -I"$root/src" \
  -dM -E -include lj_arch.h -x c /dev/null >"$pauth_macros"
grep -E '^#define LJ_ABI_PAUTH[[:space:]]+1$' \
  "$pauth_macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED[[:space:]]+1$' \
  "$pauth_macros" >/dev/null
grep -E '^#define LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED[[:space:]]+1$' \
  "$pauth_macros" >/dev/null

otool -tvV "$vm_object" >"$pauth_vm_disasm"
awk '/^_lj_BC_JLOOP:/ { copy=1 }
     copy { print }
     copy && /^_lj_BC_JMP:/ { exit }' \
  "$pauth_vm_disasm" >"$pauth_jloop_region"
test -s "$pauth_jloop_region"
grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$pauth_jloop_region" >/dev/null
if grep -E 'braa[[:space:]]|br[[:space:]]+x1$|sub[[:space:]]+sp, sp, #0x10' \
     "$pauth_jloop_region" >/dev/null; then
  echo "closed ARM64e JLOOP still contains a native trace-entry path" >&2
  exit 1
fi

# Exercise a genuinely hot interpreter loop with JIT enabled. The closed root
# recorder must leave the trace table empty, and completing the process proves
# that the arm64e VM does not stray into an unauthenticated native target.
env LUA_PATH="$root/src/?.lua;$root/src/jit/?.lua;;" "$root/src/luajit" -e '
  local util = require("jit.util")
  jit.flush(); jit.on(); assert(jit.status())
  jit.opt.start("hotloop=1", "hotexit=1", "maxtrace=2")
  local function f(n)
    local i, x = 0, 0
    while i < n do i = i + 1; x = x + i end
    return x
  end
  for _ = 1, 8 do assert(f(20) == 210) end
  assert(util.traceinfo(1) == nil,
         "closed arm64e root recorder unexpectedly published a trace")
'

# Leave the isolated checkout in its ordinary native experimental mode.
env MACOSX_DEPLOYMENT_TARGET="$minver" make -C "$root/src" clean
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" XCFLAGS="$xcflags"

echo "arm64_jit_root_entry_contract OK: ordinary strict-loop entry and ARM64e fail-closed interpreter verified"
