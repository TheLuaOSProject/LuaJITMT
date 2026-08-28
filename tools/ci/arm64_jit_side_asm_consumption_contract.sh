#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_side_asm_consumption_contract SKIP: requires native macOS arm64"
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
probe_xcflags="$ordinary_xcflags -DLJ_ARM64_SIDE_ASM_TEST"
pauth_probe_xcflags="$probe_xcflags -DLUAJIT_ENABLE_CET_BR"
archive=$root/src/libluajit.a
fixture_source=$root/tests/t-arm64-jit-side-asm-consumption.c
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
    echo "ARM64 side-assembler contract could not restore arm64 build" >&2
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
      echo "ARM64 side-assembler contract lock timed out: $lock_dir" >&2
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
test -f "$fixture_source" || {
  echo "missing ARM64 side-assembler fixture: $fixture_source" >&2
  exit 1
}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-side-asm.XXXXXX")
fixture=$tmpdir/t-arm64-jit-side-asm-consumption
fixture_obj=$tmpdir/t-arm64-jit-side-asm-consumption.o
macros=$tmpdir/macros-arm64.txt
pauth_fixture=$tmpdir/t-arm64-jit-side-asm-consumption-arm64e
pauth_obj=$tmpdir/t-arm64-jit-side-asm-consumption-arm64e.o
pauth_macros=$tmpdir/macros-arm64e.txt
bad_macros=$tmpdir/bad-macros.txt
compact_scratch_region=$tmpdir/trace-compact-scratch.txt
compact_plan_region=$tmpdir/trace-compact-plan.txt
compact_init_region=$tmpdir/trace-compact-init.txt
compact_reset_region=$tmpdir/trace-compact-reset.txt
trace_save_region=$tmpdir/trace-save.txt
restore_needed=1

# The bypass is deliberately impossible in an ordinary helper build, and the
# special build is valid only while production side recording remains closed.
if "$cc" -arch arm64 -mmacosx-version-min="$minver" \
     -DLUAJIT_MT_ARM64_BOOTSTRAP \
     -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL \
     -DLJ_ARM64_SIDE_ASM_TEST -I"$root/src" \
     -E -x c -include lj_arch.h /dev/null >"$bad_macros" 2>&1; then
  echo "ARM64 side-assembler probe compiled without trace test helpers" >&2
  exit 1
fi
grep -F 'LJ_ARM64_SIDE_ASM_TEST requires LJ_TRACE_TEST_HELPERS' \
  "$bad_macros" >/dev/null

for required in \
  'defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_ARM64_SIDE_ASM_TEST)' \
  'LJ_ARM64_SIDE_ASM_TEST requires closed ARM64 side recording' \
  'asm_test_side_probe_capture(J);' \
  'asm_test_side_probe_parentmap(as->parentmap, as->parentmap_n);' \
  'asm_test_side_probe_note(LJ_ARM64_SIDE_ASM_PROBE_PREHEAD);' \
  'asm_test_side_probe_postra(as->mcp,' \
  'asm_test_side_probe_tail(certified_parent_mcode, tail_pc);' \
  'asm_test_side_probe_note(LJ_ARM64_SIDE_ASM_PROBE_FINAL);' \
  'asm_test_side_probe_note(LJ_ARM64_SIDE_ASM_PROBE_MARKER);' \
  'lj_trace_test_arm64_side_compact_roundtrip(J, T,' \
  'LJ_ARM64_SIDE_ASM_PROBE_COMPACT' \
  'lj_trace_test_arm64_side_publish_seal(J, T)' \
  'LJ_ARM64_SIDE_ASM_PROBE_SEAL' \
  '(void)asm_test_side_probe_finish(J, T);' \
  'lj_trace_err(J, LJ_TRERR_NYIIR);'; do
  grep -F "$required" "$root/src/lj_asm.c" "$root/src/lj_arch.h" \
    >/dev/null || {
    echo "ARM64 side-assembler probe lost containment/proof: $required" >&2
    exit 1
  }
done

# Freeze the resumed-CGET grammar consumed by both real ARM64 assembler gates.
# The semantic CGET is a NOP because its value is already inherited from the
# parent; ADD therefore consumes the parent value directly. The real POSTRA
# stage below binds this grammar to the observed register allocation.
for required in \
  'ARM64_SIDE_R_CGET = REF_BASE+2u,' \
  'ARM64_SIDE_R_ADD = REF_BASE+3u,' \
  'static const MSize mapofs[5] = { 0, 3, 7, 11, 14 };' \
  'static const uint8_t nent[5] = { 1, 2, 2, 1, 1 };' \
  'static const uint8_t nslots[5] = { 5, 6, 6, 5, 5 };' \
  'static const MSize pcpos[5] = { 13, 14, 3, 17, 7 };' \
  'view->nk != ARM64_SIDE_K_ONE || view->nsnap != 5u ||' \
  'view->nsnapmap != 17u || view->baseslot != 1u+LJ_FR2 ||' \
  'ARM64_SIDE_REQUIRE(ARM64_SIDE_R_CGET, IR_NOP, IRT_NIL, 0, 0);' \
  'ARM64_SIDE_R_PARENT, ARM64_SIDE_K_ONE);' \
  'RID_X27, RID_INIT, RID_X28, RID_X27' \
  'view->parentmap[0] != REGSP(RID_X28, SPS_NONE)' \
  'A64F_D(RID_X27) | A64F_M(RID_X28)) ||'; do
  grep -F "$required" "$root/src/lj_asm.c" >/dev/null || {
    echo "ARM64 resumed-CGET assembler shape changed: $required" >&2
    exit 1
  }
done
if grep -F 'ARM64_SIDE_R_VALUE' "$root/src/lj_asm.c" >/dev/null; then
  echo "ARM64 side assembler regained the pre-CGET semantic value" >&2
  exit 1
fi

# The ordinary trace-save path and the future bounded first-child path share
# one private compact-body constructor. Planning is read-only; initialization
# and rollback cannot publish, allocate, wait, invoke callbacks or alter parent
# topology. trace_save itself remains only an ownership/publication suffix.
awk '/^static void trace_compact_scratch_init\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$compact_scratch_region"
awk '/^static int trace_compact_body_plan\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$compact_plan_region"
awk '/^static void trace_compact_body_init\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$compact_init_region"
awk '/^static void trace_compact_body_reset\(/ { copy=1 }
     copy { print }
     copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$compact_reset_region"
sed -n '/^static void trace_save(/,/^int LJ_FASTCALL lj_trace_free_gc(/p' \
  "$root/src/lj_trace.c" >"$trace_save_region"
for region in "$compact_scratch_region" "$compact_plan_region" \
  "$compact_init_region" "$compact_reset_region" "$trace_save_region"; do
  test -s "$region" || {
    echo "ARM64 compact-body contract region is empty: $region" >&2
    exit 1
  }
done
for required in \
  'body != J->curfinal' \
  'local.nins != trace_nins_acq(source)' \
  'local.nk != trace_nk_acq(source)' \
  'local.nsnap != trace_nsnap_acq(source)' \
  'local.nsnapmap != trace_nsnapmap_acq(source)' \
  '!trace_size_checked(J2G(J), body, &size, &checked_nsnap)' \
  '&local.ir[local.nk] != (IRIns *)p' \
  'size != expected' \
  '*plan = local;'; do
  grep -F "$required" "$compact_plan_region" >/dev/null || {
    echo "ARM64 compact-body planning invariant changed: $required" >&2
    exit 1
  }
done
for required in \
  'memcpy(T, source, sizeof(GCtrace));' \
  'memcpy(plan->snap, plan->source_snap,' \
  'memcpy(plan->snapmap, plan->source_snapmap,' \
  'la_storeptr_rel((void **)&T->snap, plan->snap);' \
  'la_storeptr_rel((void **)&T->snapmap, plan->snapmap);' \
  'ptrauth_nop_cast(ASMFunction, trace_mcode_acq(source)), T)' \
  'trace_retired_link_unlinked_rel(T);'; do
  grep -F "$required" "$compact_init_region" >/dev/null || {
    echo "ARM64 compact-body initializer invariant changed: $required" >&2
    exit 1
  }
done
for required in \
  'trace_compact_scratch_init(plan->body, plan->ir, plan->nins, plan->nk,' \
  'setgcrefnullrel(T->nextgc);' \
  'lj_obj_setgcflags(obj2gco(T), 0);' \
  'la_storeptr_rel((void **)&T->snap, NULL);' \
  'trace_startpt_clear(T);' \
  'trace_exittab_rel(T, NULL);' \
  'trace_exitstub_rel(T, NULL);' \
  'trace_nchild_rel(T, 0);' \
  'trace_nextside_rel(T, 0);' \
  'la_store32_rel(&T->native_pins, 0);' \
  'la_store64_rel(&T->retire_epoch, 0);' \
  'trace_retired_link_unlinked_rel(T);'; do
  grep -F "$required" "$compact_reset_region" "$compact_scratch_region" \
    >/dev/null || {
    echo "ARM64 compact-body reset invariant changed: $required" >&2
    exit 1
  }
done
for forbidden in \
  'lj_gc_linkobj' 'traceslot_' 'lj_gc_pubtrace' 'lj_mcode_commit' \
  'lj_mcode_publish' 'lj_mcode_sync' 'lj_gc2_smr_' 'lj_mem_' \
  'perftools_' 'lj_vmevent' 'lj_trace_state' 'lj_gdbjit_addtrace' \
  'lj_gdbjit_preparetrace' 'lj_gdbjit_committrace' \
  'J->curfinal =' 'J->cur.traceno =' 'J->cur.exittab =' \
  'J->cur.exitstub =' \
  'trace_nextside_cas' 'trace_exittarget_arm64_raw_cas'; do
  if grep -F "$forbidden" "$compact_scratch_region" \
       "$compact_plan_region" "$compact_init_region" \
       "$compact_reset_region" >/dev/null; then
    echo "ARM64 private compact-body step gained forbidden action: $forbidden" >&2
    exit 1
  fi
done
test "$(grep -Fc 'trace_compact_body_plan(J, T, &plan)' \
  "$trace_save_region")" = 1
test "$(grep -Fc 'trace_compact_body_init(J, &plan);' \
  "$trace_save_region")" = 1
test "$(grep -Fc 'abort();' "$trace_save_region")" = 1
for forbidden in \
  'memcpy(T, &J->cur, sizeof(GCtrace))' 'newwhite' 'TRACE_APPENDVEC' \
  'mcauth =' 'trace_mcauth' 'la_storefunc_rel(&T->mcauth' \
  'T->snap' 'T->snapmap' 'memcpy(' 'trace_compact_body_reset'; do
  if grep -F "$forbidden" "$trace_save_region" >/dev/null; then
    echo "trace_save regained private compaction operation: $forbidden" >&2
    exit 1
  fi
done
test "$(grep -Fc 'TRACE_APPENDVEC' "$root/src/lj_trace.c")" = 0
test "$(grep -Fc 'memcpy(T, &J->cur, sizeof(GCtrace))' \
  "$root/src/lj_trace.c")" = 0
test "$(grep -Fc 'memcpy(T, source, sizeof(GCtrace));' \
  "$root/src/lj_trace.c")" = 1
compact_init_line=$(grep -n 'trace_compact_body_init(J, &plan);' \
  "$trace_save_region" | cut -d: -f1)
owner_clear_line=$(grep -n 'J->cur.traceno = 0;' "$trace_save_region" | \
  cut -d: -f1)
exittab_clear_line=$(grep -n 'J->cur.exittab = NULL;' \
  "$trace_save_region" | cut -d: -f1)
exitstub_clear_line=$(grep -n 'J->cur.exitstub = NULL;' \
  "$trace_save_region" | cut -d: -f1)
curfinal_clear_line=$(grep -n 'J->curfinal = NULL;' \
  "$trace_save_region" | cut -d: -f1)
root_publish_line=$(grep -n 'lj_gc_linkobj_new(g, obj2gco(T));' \
  "$trace_save_region" | cut -d: -f1)
slot_publish_line=$(grep -n 'traceslot_publish(J, T->traceno, T);' \
  "$trace_save_region" | cut -d: -f1)
pubtrace_line=$(grep -n 'lj_gc_pubtrace(g, T->traceno);' \
  "$trace_save_region" | cut -d: -f1)
gdb_publish_line=$(grep -n 'lj_gdbjit_addtrace(J, T);' \
  "$trace_save_region" | cut -d: -f1)
perf_publish_line=$(grep -n 'perftools_addtrace(J, T);' \
  "$trace_save_region" | cut -d: -f1)
test "$compact_init_line" -lt "$owner_clear_line"
test "$owner_clear_line" -lt "$exittab_clear_line"
test "$exittab_clear_line" -lt "$exitstub_clear_line"
test "$exitstub_clear_line" -lt "$curfinal_clear_line"
test "$curfinal_clear_line" -lt "$root_publish_line"
test "$root_publish_line" -lt "$slot_publish_line"
test "$slot_publish_line" -lt "$pubtrace_line"
test "$pubtrace_line" -lt "$gdb_publish_line"
test "$gdb_publish_line" -lt "$perf_publish_line"
for required in \
  'LJ_TRACE_PUBLISH' \
  'if (old == (uint32_t)LJ_TRACE_PUBLISH)' \
  'static LJ_AINLINE int lj_trace_state_publish_try(jit_State *J)' \
  'trace_arm64_side_publish_child_valid(' \
  'trace_arm64_side_publish_body_alloc_valid(' \
  'trace_nsnap_acq(body) != trace_nsnap_acq(T)' \
  'trace_nsnapmap_acq(body) != trace_nsnapmap_acq(T)' \
  '!trace_size_checked(J2G(J), body, &size, &nsnap)' \
  'gcref_acq(cert->tracev->slot[cert->child])' \
  'fallback_encoding_bits =' \
  'trace_arm64_first_side_pointer_bits(' \
  'plan->parent_fallback_encoding_bits =' \
  'plan->parent_fallback_encoding = parentview->fallback_encoding;' \
  'trace_arm64_side_parent_revalidate_held(J, &cert, &parentview)' \
  'lj_trace_state_publish_try(J)' \
  'if (result != LJ_TRACE_ARM64_SIDE_PARENT_OK)' \
  'gc2_smr_readers_acq(J2G(J)) == 1' \
  'trace_arm64_side_publish_raw_negative_test(J, T)' \
  'ptrauth_blend_discriminator((void *)J, (uintptr_t)salt)' \
  'ptrauth_nop_cast(MCode *, lj_ptr_strip(signedwrong)) != fallback' \
  'trace_test_arm64_side_publish_seal_failure) == 5u' \
  'lj_trace_test_arm64_side_publish_raw_negative' \
  'lj_trace_state_abort(J);'; do
  grep -F "$required" "$root/src/lj_trace.c" "$root/src/lj_trace.h" \
    "$root/src/lj_jit.h" >/dev/null || {
    echo "ARM64 side publication-seal invariant changed: $required" >&2
    exit 1
  }
done
grep -F 'defined(LJ_ARM64_SIDE_ASM_TEST)' "$root/src/lj_trace.c" >/dev/null
grep -F 'lj_asm_arm64_test_side_probe_ingress(parent, exitno)' \
  "$root/src/lj_trace.c" >/dev/null
for required in \
  'static int trace_arm64_side_asm_test_preflight(jit_State *J,' \
  'lj_asm_arm64_test_side_probe_active(J->parent, J->exitno)' \
  'lj_trace_arm64_first_side_loop_valid(' \
  'LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER' \
  'if (J->parent != 0) {' \
  'trace_arm64_side_asm_test_preflight(J, pc, &L)'; do
  grep -F "$required" "$root/src/lj_trace.c" >/dev/null || {
    echo "ARM64 side-assembler recorder preflight changed: $required" >&2
    exit 1
  }
done

for required in \
  "jit.opt.start('hotloop=1','hotexit=1','maxtrace=3')" \
  'if bias~=0 then i=i+1 end' \
  'PROBE_CHILD_EXIT = 3,' \
  'PROBE_CHILD_R_CGET = REF_BASE+2u,' \
  'PROBE_CHILD_R_ADD = REF_BASE+3u,' \
  'PROBE_CHILD_NSNAP = 5,' \
  'PROBE_CHILD_NSNAPMAP = 17,' \
  'static const MSize mapofs[PROBE_CHILD_NSNAP] = { 0, 3, 7, 11, 14 };' \
  'static const uint8_t nent[PROBE_CHILD_NSNAP] = { 1, 2, 2, 1, 1 };' \
  'static const uint8_t nslots[PROBE_CHILD_NSNAP] = { 5, 6, 6, 5, 5 };' \
  'static const MSize pcpos[PROBE_CHILD_NSNAP] = { 13, 14, 3, 17, 7 };' \
  'EXPECT_ABORTED_CHILD_IR(PROBE_CHILD_R_CGET, IR_NOP, IRT_NIL, 0, 0);' \
  'IRT_INT|IRT_GUARD, PROBE_CHILD_R_PARENT, PROBE_CHILD_K_ONE);' \
  'assert(trace_nsnap_acq(child) == PROBE_CHILD_NSNAP);' \
  'assert(trace_nsnapmap_acq(child) == PROBE_CHILD_NSNAPMAP);' \
  'assert(snap_ref_acq(&snap[PROBE_CHILD_EXIT]) == PROBE_CHILD_R_GT);' \
  'assert(lj_trace_test_exittab_last_alloc_slots() == 6);' \
  'assert(lj_trace_test_exittab_last_free_slots() == 6);' \
  'expect_aborted_child_shape(J, pt, continuation);' \
  'lj_trace_test_reset_exit_stats();' \
  'lj_trace_test_reset_exittab_stats();' \
  'lj_asm_arm64_test_side_probe_arm(PROBE_PARENT, PROBE_EXIT);' \
  'lj_asm_arm64_test_force_exitstub_mcode_retry(1);' \
  'assert(probe.stages == LJ_ARM64_SIDE_ASM_PROBE_ALL);' \
  'assert((probe.stages & LJ_ARM64_SIDE_ASM_PROBE_PREHEAD) != 0);' \
  'assert((probe.stages & LJ_ARM64_SIDE_ASM_PROBE_POSTRA) != 0);' \
  'assert(probe.compact_geometry_reject == 1);' \
  'assert(probe.compact_init == 1);' \
  'assert(probe.compact_reset == 1);' \
  'assert(probe.compact_pauth == (uint32_t)LJ_ABI_PAUTH);' \
  'assert(probe.seal_failure == 0);' \
  'assert(probe.raw_negative == (uint32_t)LJ_ABI_PAUTH);' \
  'assert(probe.capture_count == 2);' \
  'assert(probe.parent == PROBE_PARENT);' \
  'assert(probe.child == PROBE_CHILD);' \
  'assert(probe.exitno == PROBE_EXIT);' \
  'assert(probe.cert_tracev == tracevec_acq(J));' \
  'assert(probe.cert_body == root);' \
  'assert(probe.cert_mcode == root_mcode);' \
  'assert(probe.cert_continuation == continuation);' \
  'assert(probe.cert_child == PROBE_CHILD);' \
  'assert(probe.parentmap0 == REGSP(RID_X28, SPS_NONE));' \
  'assert(probe.entry[0] == A64I_LE(A64I_BTI_J));' \
  'A64F_D(RID_X27) | A64F_M(RID_X28)' \
  'assert(probe.tail_target == root_mcode);' \
  'assert(lj_asm_arm64_b26_encode(' \
  'assert(probe.tail_ins == expected_tail);' \
  'assert(probe.marker == TRACE_ARM64_INT_SIDE_ADMITTED);' \
  'assert(lj_trace_test_mcode_retries() == 1);' \
  'assert(lj_trace_test_abort_count() == 2);' \
  'assert(lj_trace_test_exittab_allocs() == 2);' \
  'assert(lj_trace_test_exittab_frees() == 2);' \
  'assert(slot2 == NULL);' \
  'assert(J->curfinal == NULL);' \
  'assert(L->cframe == saved_cframe);' \
  'assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);' \
  'assert(lj_tg_load_jit_base(tg) == NULL);' \
  'assert(side_parent_cert_zero(&J->arm64_side_parent));' \
  'assert(gc2_smr_readers_acq(g) == 0);' \
  'assert(jit_token_acq(g) == 0);' \
  'assert(jit_owner_l_acq(J) == NULL);' \
  'assert(!lj_trace_state_publish_try(J));' \
  'assert(lj_trace_state_publish_try(J));' \
  'assert(lj_trace_state_load(J) == LJ_TRACE_PUBLISH);' \
  'assert(trace_nchild_acq(root) == root_nchild && root_nchild == 0);' \
  'assert(trace_nextside_acq(root) == root_nextside && root_nextside == 0);' \
  'assert(snap_count_acq(&root_snap[PROBE_EXIT]) != SNAPCOUNT_DONE);' \
  'assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_fallback);' \
  'assert(call_probe(L, 3, 0) == 3);'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 side-assembler fixture lost required proof: $required" >&2
    exit 1
  }
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$probe_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$probe_xcflags"

# shellcheck disable=SC2086 # probe_xcflags intentionally expands.
"$cc" -arch arm64 -mmacosx-version-min="$minver" $probe_xcflags \
  -I"$root/src" -dM -E -x c -include lj_arch.h /dev/null >"$macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 0' \
  'LJ_ABI_BRANCH_TRACK 0' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_SIDE_ASM_TEST 1'; do
  grep -E "^#define ${setting}$" "$macros" >/dev/null || {
    echo "ARM64 side-assembler probe gate mismatch: $setting" >&2
    exit 1
  }
done
test "$(lipo -archs "$archive")" = arm64
if nm -g "$archive" | grep -E '_trace_compact_(scratch|body)_' >/dev/null; then
  echo "private compact-body helper escaped the LuaJIT archive" >&2
  exit 1
fi
for symbol in \
  _lj_asm_arm64_test_side_probe_arm \
  _lj_asm_arm64_test_side_probe_ingress \
  _lj_asm_arm64_test_side_probe_active \
  _lj_asm_arm64_test_side_probe_read \
  _lj_trace_test_arm64_side_compact_roundtrip \
  _lj_trace_test_arm64_side_publish_seal \
  _lj_trace_test_arm64_side_publish_seal_failure \
  _lj_trace_test_arm64_side_publish_raw_negative; do
  nm "$archive" | grep -F " T $symbol" >/dev/null || {
    echo "ARM64 side-assembler probe archive lost $symbol" >&2
    exit 1
  }
done

# shellcheck disable=SC2086 # probe_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $probe_xcflags -I"$root/src" \
  -c "$fixture_source" -o "$fixture_obj"
"$cc" -arch arm64 -mmacosx-version-min="$minver" \
  "$fixture_obj" "$archive" -lm -pthread -o "$fixture"
otool -hv "$fixture" | grep -E 'ARM64[[:space:]]+ALL' >/dev/null

ordinary_runs=${LJ_ARM64_SIDE_ASM_RUNS:-2}
run=1
while test "$run" -le "$ordinary_runs"; do
  "$fixture"
  run=$((run+1))
done

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_probe_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" \
    TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' \
    XCFLAGS="$pauth_probe_xcflags"

# shellcheck disable=SC2086 # pauth_probe_xcflags intentionally expands.
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" $pauth_probe_xcflags -I"$root/src" \
  -dM -E -x c -include lj_arch.h /dev/null >"$pauth_macros"
for setting in \
  'LJ_TARGET_ARM64 1' \
  'LJ_ABI_PAUTH 1' \
  'LJ_ABI_BRANCH_TRACK 1' \
  'LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED 1' \
  'LJ_ARM64_SIDE_ASM_TEST 1'; do
  grep -E "^#define ${setting}$" "$pauth_macros" >/dev/null || {
    echo "ARM64e side-assembler probe gate mismatch: $setting" >&2
    exit 1
  }
done
test "$(lipo -archs "$archive")" = arm64e

# shellcheck disable=SC2086 # pauth_probe_xcflags intentionally expands.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64e \
  -mbranch-protection=bti -mmacosx-version-min="$minver" \
  $pauth_probe_xcflags -I"$root/src" \
  -c "$fixture_source" -o "$pauth_obj"
"$cc" -arch arm64e -mbranch-protection=bti \
  -mmacosx-version-min="$minver" "$pauth_obj" "$archive" -lm -pthread \
  -o "$pauth_fixture"
otool -hv "$pauth_fixture" | grep -E 'ARM64[[:space:]]+E' >/dev/null

pauth_runs=${LJ_ARM64_SIDE_ASM_PAUTH_RUNS:-2}
run=1
while test "$run" -le "$pauth_runs"; do
  "$pauth_fixture"
  run=$((run+1))
done

# Leave the shared checkout in the ordinary experimental configuration.
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_FLAGS='-arch arm64' \
    XCFLAGS="$ordinary_xcflags"
if nm "$archive" | grep -E \
     '_lj_asm_arm64_test_side_probe_(arm|ingress|active|read)$|_lj_trace_test_arm64_side_(compact_roundtrip|publish_(seal(_failure)?|raw_negative))$' \
     >/dev/null; then
  echo "ordinary ARM64 helper build retained special side-probe APIs" >&2
  exit 1
fi
restore_needed=0

echo "arm64_jit_side_asm_consumption_contract OK: resumed-CGET first-side assembly, exact semantic snapshots/post-RA gate, compact-body init/reset, MCODELM recapture and raw ARM64e negative dry publication seal proved without publication"
