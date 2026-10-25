#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_root_entry_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_root_entry_contract
lj_ci_tmpdir lj-arm64-root-entry
tmpdir=$LJ_CI_TMPDIR
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLJ_ARM64_EXIT_TEST_HELPERS'
pauth_xcflags="$xcflags -DLUAJIT_ENABLE_CET_BR"
lj_ci_set_restore "${LJ_TEST_RESTORE_TARGET_FLAGS:--arch arm64}" \
  "${LJ_TEST_RESTORE_XCFLAGS:-$xcflags}"

root_fixture=$root/tests/t-arm64-jit-root-entry.c
side_fixture=$root/tests/t-arm64-jit-side-adaptation.c
test -f "$root_fixture" && test -f "$side_fixture"

check_root_entry_order() {
  helper=$tmpdir/entry-helper.c
  child=$tmpdir/entry-child.c
  equal=$tmpdir/entry-child-equal.c
  awk '/^lj_trace_enter_root\(/ { copy=1 }
       copy { print }
       copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$helper"
  awk '/^static int trace_root_entry_first_child_view_acq/ { copy=1 }
       copy { print }
       copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$child"
  awk '/^static int trace_root_entry_first_child_view_equal/ { copy=1 }
       copy { print }
       copy && /^}/ { exit }' "$root/src/lj_trace.c" >"$equal"
  test -s "$helper" && test -s "$child" && test -s "$equal"
  line() { grep -nF "$1" "$helper" | sed -n "${2:-1}p" | cut -d: -f1; }
  source_gate=$(line '!trace_root_entry_source_admitted(sourceop)')
  publish=$(line 'lj_tg_store_jit_base(tg, base)')
  fence=$(line 'la_fence_seq()')
  gate=$(line 'lj_gc2_jit_entry_open(g)' 2)
  first_slot=$(line 'trace_root_entry_slot_acq(J, traceno' 1)
  first_layout=$(line 'trace_root_entry_arm64_layout_valid(&view, T, sourceins)')
  first_child=$(line 'trace_root_entry_first_child_view_acq(' 1)
  second_slot=$(line 'trace_root_entry_slot_acq(J, traceno' 2)
  equal_child=$(line 'trace_root_entry_first_child_view_equal(&childview, &childview2)')
  postmetadata=$(line 'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA')
  postadmission=$(line 'LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION')
  cleanup=$(line 'lj_tg_store_jit_base(tg, NULL)')
  test -n "$source_gate" && test -n "$publish" && test -n "$fence" &&
    test -n "$gate" && test -n "$first_slot" && test -n "$first_layout" &&
    test -n "$first_child" && test -n "$second_slot" &&
    test -n "$equal_child" && test -n "$postmetadata" &&
    test -n "$postadmission" && test -n "$cleanup"
  test "$source_gate" -lt "$publish" && test "$publish" -lt "$fence" &&
    test "$fence" -lt "$gate" && test "$gate" -lt "$first_slot" &&
    test "$first_slot" -lt "$first_layout" &&
    test "$first_layout" -lt "$first_child" &&
    test "$first_child" -lt "$second_slot" &&
    test "$second_slot" -lt "$equal_child" &&
    test "$equal_child" -lt "$postmetadata" &&
    test "$postmetadata" -lt "$postadmission" &&
    test "$postadmission" -lt "$cleanup"

  # The selected exit must come from the child IR, be bounded against both
  # parent arrays before indexing, and be re-read after independent post-RA
  # admission. Do not pin this contract to one trace shape or exit number.
  base=$(grep -nF 'base = ir_load_acq(&view->ir[REF_BASE]);' "$child" |
    sed -n '1p' | cut -d: -f1)
  select=$(grep -nF 'parent_exitno = (ExitNo)base.op2;' "$child" |
    sed -n '1p' | cut -d: -f1)
  snap_bound=$(grep -nF '(MSize)base.op2 >= rootview->nsnap' "$child" |
    sed -n '1p' | cut -d: -f1)
  edge_bound=$(grep -nF '(MSize)base.op2 >= parent_nexits' "$child" |
    sed -n '1p' | cut -d: -f1)
  snapshot=$(grep -nF '&rootview->snap[parent_exitno]' "$child" |
    sed -n '1p' | cut -d: -f1)
  edge1=$(grep -nF 'rootview->exittab[parent_exitno]' "$child" |
    sed -n '1p' | cut -d: -f1)
  postra=$(grep -nF 'lj_asm_arm64_side_postra_admit(&postra' "$child" |
    sed -n '1p' | cut -d: -f1)
  edge2=$(grep -nF 'rootview->exittab[parent_exitno]' "$child" |
    sed -n '2p' | cut -d: -f1)
  for number in "$base" "$select" "$snap_bound" "$edge_bound" \
    "$snapshot" "$edge1" "$postra" "$edge2"; do
    test -n "$number"
  done
  test "$base" -lt "$select" && test "$select" -lt "$snap_bound" &&
    test "$select" -lt "$edge_bound" && test "$snap_bound" -lt "$snapshot" &&
    test "$edge_bound" -lt "$edge1" && test "$edge1" -lt "$postra" &&
    test "$postra" -lt "$edge2"
  for required in trace_size_checked trace_body_refs_valid \
      trace_arm64_side_certificate_valid_held trace_nextside_acq \
      trace_nchild_acq trace_mcloop_acq trace_retired_link_acq; do
    grep -F "$required" "$child" >/dev/null
  done
  grep -F 'a->parent_exitno == b->parent_exitno' "$equal" >/dev/null
  if grep -E 'lj_gc2_smr|lj_mem_|lj_alloc|lj_err_|lj_vmevent|lj_safepoint' \
      "$helper" >/dev/null; then
    echo "root-entry hot helper gained a blocking or allocating operation" >&2
    exit 1
  fi
}

check_vm_entry_order() {
  vm=$1
  tag=$2
  disasm=$tmpdir/vm-$tag.disasm
  reloc=$tmpdir/vm-$tag.reloc
  loop=$tmpdir/vm-$tag-loop.txt
  funcf=$tmpdir/vm-$tag-funcf.txt
  otool -rv "$vm" >"$reloc"
  test "$(grep -c 'BR26.*_lj_trace_enter_root$' "$reloc")" = 3
  otool -tvV "$vm" >"$disasm"
  awk '/^_lj_BC_JLOOP:/ { copy=1 }
       copy { print }
       copy && /^_lj_BC_JMP:/ { exit }' "$disasm" >"$loop"
  awk '/^_lj_BC_JFUNCF:/ { copy=1 }
       copy { print }
       copy && /^_lj_BC_JFUNCV:/ { exit }' "$disasm" >"$funcf"
  test -s "$loop" && test -s "$funcf"
  if test "$tag" = arm64e; then
    grep -E 'bti[[:space:]]+j' "$loop" >/dev/null
    grep -E 'braa[[:space:]]+x1, x0$' "$loop" >/dev/null
  else
    grep -E 'br[[:space:]]+x1$' "$loop" >/dev/null
  fi
  grep -E 'sub[[:space:]]+sp, sp, #0x10' "$loop" >/dev/null
  call=$(grep -nE 'bl[[:space:]]+0x' "$loop" | sed -n '1p' | cut -d: -f1)
  reserve=$(grep -nE 'sub[[:space:]]+sp, sp, #0x10' "$loop" |
    sed -n '1p' | cut -d: -f1)
  branch=$(grep -nE 'br(a|aa)[[:space:]]+x1' "$loop" |
    sed -n '1p' | cut -d: -f1)
  test -n "$call" && test -n "$reserve" && test -n "$branch" &&
    test "$call" -lt "$reserve" && test "$reserve" -lt "$branch"
  if grep -E 'stlr[[:space:]]+xzr, \[x14\]' "$loop" >/dev/null; then
    echo "open JLOOP unexpectedly clears its entry lease" >&2
    exit 1
  fi
  grep -E 'ldar[[:space:]]+w16, \[x14\]' "$funcf" >/dev/null
  grep -E 'sub[[:space:]]+sp, sp, #0x10' "$funcf" >/dev/null
  if test "$tag" = arm64e; then
    grep -E 'braa[[:space:]]+x1, x0$' "$funcf" >/dev/null
  else
    grep -E 'br[[:space:]]+x1$' "$funcf" >/dev/null
  fi
}

run_profile() {
  arch=$1 target=$2 flags=$3 tag=$4
  lj_ci_build_profile "$arch" "$target" "$flags" "$tag"
  lj_ci_require_symbols lj_trace_enter_root lj_trace_test_root_entry_publishes
  root_bin=$tmpdir/root-entry-$tag
  side_bin=$tmpdir/side-adaptation-$tag
  lj_ci_compile_fixture "$root_bin" "$root_fixture"
  lj_ci_compile_fixture "$side_bin" "$side_fixture" \
    -Wno-unused-but-set-variable
  lj_ci_run_fixture 120 "$root_bin"
  lj_ci_run_fixture 120 "$side_bin"
  check_vm_entry_order "$root/src/lj_vm.o" "$tag"
}

check_root_entry_order
run_profile arm64 '-arch arm64' "$xcflags" arm64
run_profile arm64e '-arch arm64e -mbranch-protection=bti' \
  "$pauth_xcflags" arm64e

# Run the runtime certificate mutations without assertions too: release entry
# must still reject stale identity, generation and lifetime authorities.
release_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLJ_TRACE_TEST_HELPERS -DLJ_ARM64_EXIT_TEST_HELPERS'
lj_ci_build_profile arm64 '-arch arm64' "$release_xcflags" release
release_bin=$tmpdir/root-entry-release
lj_ci_compile_fixture "$release_bin" "$root_fixture"
lj_ci_run_fixture 120 "$release_bin"

lj_ci_restore "$LJ_CI_RESTORE_TARGET_FLAGS" "$LJ_CI_RESTORE_XCFLAGS"
echo "arm64_jit_root_entry_contract OK: root, generic first-child revalidation, ARM64/arm64e entry ordering and release certificate rejection"
