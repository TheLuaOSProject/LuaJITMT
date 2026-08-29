#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
vm_source=$root/src/vm_arm64.dasc
fixture_source=$root/tests/t-gc2-interp-hard-check.c

line_of() {
  printf '%s\n' "$1" | grep -nF "$2" | head -n 1 | cut -d: -f1
}

predicate=$(sed -n \
  '/^[|][.]macro arm64_vm_gc_should_step,/,/^[|][.]endmacro/p' \
  "$vm_source")
test -n "$predicate"

for required in \
  'arm64_vm_u64_acq tmp1, g, GL_OFS(gc.total)' \
  'arm64_vm_u64_acq tmp2, g, GL_OFS(gc.threshold)' \
  'arm64_vm_u64_acq tmp1, g, GL_OFS(gc2.alloc_since_trigger)' \
  'arm64_vm_u64_acq tmp2, g, GL_OFS(gc2.hard_bytes)' \
  'cmp tmp1, tmp2' \
  'bls done' \
  'cmp tmp2, #LJ_GC2_ACCT_FLUSH' \
  'blo target' \
  'DISPATCH_TG(local_total)&0xfff000' \
  'DISPATCH_TG(local_total)&0xfff' \
  'ldar tmp1, [ATMP]' \
  'cmp tmp1, #LJ_GC2_ACCT_FLUSH' \
  'bhs target'; do
  printf '%s\n' "$predicate" | grep -F "$required" >/dev/null || {
    echo "ARM64 GC-step predicate lost: $required" >&2
    exit 1
  }
done

hard_count=$(printf '%s\n' "$predicate" | \
  grep -Fc 'arm64_vm_u64_acq tmp2, g, GL_OFS(gc2.hard_bytes)' || true)
test "$hard_count" -eq 2 || {
  echo "ARM64 GC-step predicate must reacquire hard_bytes" >&2
  exit 1
}

total_line=$(line_of "$predicate" 'GL_OFS(gc.total)')
threshold_line=$(line_of "$predicate" 'GL_OFS(gc.threshold)')
since_line=$(line_of "$predicate" 'GL_OFS(gc2.alloc_since_trigger)')
hard1_line=$(printf '%s\n' "$predicate" | \
  grep -nF 'GL_OFS(gc2.hard_bytes)' | sed -n '1s/:.*//p')
hard2_line=$(printf '%s\n' "$predicate" | \
  grep -nF 'GL_OFS(gc2.hard_bytes)' | sed -n '2s/:.*//p')
local_line=$(line_of "$predicate" 'DISPATCH_TG(local_total)&0xfff000')
test "$total_line" -lt "$threshold_line" && \
  test "$threshold_line" -lt "$since_line" && \
  test "$since_line" -lt "$hard1_line" && \
  test "$hard1_line" -lt "$hard2_line" && \
  test "$hard2_line" -lt "$local_line"

test "$(grep -Fc 'arm64_vm_gc_should_step GLREG' "$vm_source" || true)" \
  -eq 2 || {
  echo "ARM64 fast-function and TNEW/TDUP predicates are not shared" >&2
  exit 1
}
if grep -E 'ldp .*GL->gc[.]total' "$vm_source" >/dev/null; then
  echo "ARM64 VM retains a plain paired GC threshold load" >&2
  exit 1
fi

fff_gcstep=$(sed -n '/[|]->fff_gcstep:/,/[|]\/\/-- Special dispatch/p' \
  "$vm_source")
printf '%s\n' "$fff_gcstep" | \
  grep -F 'bl extern lj_gc_step_top' >/dev/null || {
  echo "ARM64 fast-function GC landing lost lj_gc_step_top" >&2
  exit 1
}
if printf '%s\n' "$fff_gcstep" | \
   grep -E 'bl extern lj_gc_step([[:space:]]|$)' >/dev/null; then
  echo "ARM64 fast-function GC landing still calls threshold-only lj_gc_step" >&2
  exit 1
fi

for required in \
  'test_normal_hard_tnew_batch_gate' \
  'test_normal_hard_tnew_batch_boundary' \
  'test_hard_only_tdup' \
  'assert_loaded_chunk_has_op(L, BC_TDUP)' \
  'normal hard-only TNEW entered the fixtop helper before local batch debt' \
  'normal hard-only TNEW skipped the local batch boundary' \
  'interpreted hard-only TDUP did not enter the GC2 hard check' \
  'interpreted hard-only TNEW did not enter the GC2 hard check' \
  'hard-only fast function did not enter the GC2 hard check'; do
  grep -F "$required" "$fixture_source" >/dev/null || {
    echo "ARM64 GC2 runtime fixture lost: $required" >&2
    exit 1
  }
done

if test "${LJ_ARM64_GC2_SOURCE_ONLY:-0}" = 1; then
  echo "arm64_gc2_interp_hard_check_contract OK: source predicate present"
  exit 0
fi

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_gc2_interp_hard_check_contract SKIP: runtime proof requires native macOS arm64"
  exit 0
fi

lock_dir=$root/src/.lj-test-run.lock
lock_held=0
restore_needed=0
tmpdir=
jobs=${JOBS:-${MAKE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 2)}}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
safe_xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT'
restore_xcflags=${LJ_ARM64_GC2_RESTORE_XCFLAGS:-$safe_xcflags}
helper_define='-DLJ_GC2_TEST_HELPERS'
cc=${CC:-clang}

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
  lock_timeout=${LJ_TEST_RUN_LOCK_TIMEOUT:-900}
  lock_started=$(date +%s)
  while ! mkdir "$lock_dir" 2>/dev/null; do
    lock_now=$(date +%s)
    if test "$lock_timeout" -ge 0 && \
       test $((lock_now - lock_started)) -ge "$lock_timeout"; then
      echo "ARM64 GC2 contract lock timed out: $lock_dir" >&2
      exit 2
    fi
    sleep 0.2
  done
  lock_held=1
  printf 'cmd=%s\n' "$0" >"$lock_dir/owner" 2>/dev/null || true
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-gc2.XXXXXX")
restore_needed=1

object_region() {
  symbol=$1
  disasm=$2
  sed -n "/^_$symbol:/,/^_/p" "$disasm"
}

check_predicate_object() {
  symbol=$1
  disasm=$2
  region=$(object_region "$symbol" "$disasm")
  test -n "$region" || {
    echo "missing generated VM symbol: $symbol" >&2
    exit 1
  }
  nldar=$(printf '%s\n' "$region" | \
    grep -Ec '[[:space:]]ldar[[:space:]]+x[0-9]+, [[]x14[]]' || true)
  test "$nldar" -eq 6 || {
    echo "$symbol emitted $nldar predicate acquire loads, expected 6" >&2
    exit 1
  }
  nglobal=$(printf '%s\n' "$region" | \
    grep -Ec '[[:space:]]add[[:space:]]+x14, x22, #0x' || true)
  test "$nglobal" -eq 5 || {
    echo "$symbol emitted $nglobal global predicate addresses, expected 5" >&2
    exit 1
  }
  printf '%s\n' "$region" | \
    grep -E '[[:space:]]add[[:space:]]+x14, x25, #0x' >/dev/null || {
    echo "$symbol lost TG-local debt high address" >&2
    exit 1
  }
  printf '%s\n' "$region" | \
    grep -E '[[:space:]]add[[:space:]]+x14, x14, #0x' >/dev/null || {
    echo "$symbol lost TG-local debt low address" >&2
    exit 1
  }
  printf '%s\n' "$region" | grep -E '[[:space:]]b[.]ls[[:space:]]' >/dev/null
  printf '%s\n' "$region" | grep -E '[[:space:]]b[.]lo[[:space:]]' >/dev/null
  test "$(printf '%s\n' "$region" | \
    grep -Ec '[[:space:]]b[.]hs[[:space:]]' || true)" -ge 2
  printf '%s\n' "$region" | \
    sed -nE 's/.*add[[:space:]]+x14, x22, #(0x[[:xdigit:]]+).*/\1/p' | \
    sort | uniq -c | \
    awk '$1 == 2 { n++ } END { exit n == 1 ? 0 : 1 }' || {
    echo "$symbol did not reacquire exactly one global field" >&2
    exit 1
  }
}

run_case() {
  tag=$1
  target_flags=$2
  xcflags=$3
  expected_arch=$4
  fixture=$tmpdir/t-gc2-interp-hard-check-$tag
  disasm=$tmpdir/lj_vm-$tag.txt

  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" clean TARGET_SYS=Darwin \
      TARGET_FLAGS="$target_flags" XCFLAGS="$xcflags"
  env MACOSX_DEPLOYMENT_TARGET="$minver" \
    make -C "$root/src" -j"$jobs" TARGET_SYS=Darwin \
      TARGET_FLAGS="$target_flags" XCFLAGS="$xcflags"
  test "$(lipo -archs "$root/src/libluajit.a")" = "$expected_arch"
  test "$(lipo -archs "$root/src/lj_vm.o")" = "$expected_arch"
  if nm -u "$root/src/lj_vm.o" | \
     grep -x '_lj_gc_step' >/dev/null; then
    echo "$tag VM still imports threshold-only lj_gc_step" >&2
    exit 1
  fi
  nm -u "$root/src/lj_vm.o" | \
    grep -x '_lj_gc_step_top' >/dev/null || {
    echo "$tag VM lost lj_gc_step_top import" >&2
    exit 1
  }
  if nm -u "$root/src/lj_vm.o" | \
     grep -E '(__atomic|libatomic)' >/dev/null; then
    echo "$tag VM GC predicate imports an atomic helper" >&2
    exit 1
  fi

  otool -tvV "$root/src/lj_vm.o" >"$disasm"
  for symbol in lj_ff_string_char lj_BC_TNEW lj_BC_TDUP; do
    check_predicate_object "$symbol" "$disasm"
  done

  # shellcheck disable=SC2086 # Target/compiler flag groups are intentional.
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror $target_flags \
    -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
    "$fixture_source" "$root/src/libluajit.a" -lm -pthread -o "$fixture"
  "$fixture"
}

arm64_flags="$safe_xcflags $helper_define"
arm64e_flags="$arm64_flags -DLUAJIT_ENABLE_CET_BR"
run_case arm64 '-arch arm64' "$arm64_flags" arm64
run_case arm64e '-arch arm64e -mbranch-protection=bti' \
  "$arm64e_flags" arm64e
otool -hv "$tmpdir/t-gc2-interp-hard-check-arm64e" | \
  grep -E 'ARM64[[:space:]]+E' >/dev/null

env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" clean TARGET_SYS=Darwin \
    TARGET_FLAGS='-arch arm64' XCFLAGS="$restore_xcflags"
env MACOSX_DEPLOYMENT_TARGET="$minver" \
  make -C "$root/src" -j"$jobs" TARGET_SYS=Darwin \
    TARGET_FLAGS='-arch arm64' XCFLAGS="$restore_xcflags"
restore_needed=0

echo "arm64_gc2_interp_hard_check_contract OK: arm64 and arm64e/BTI VM hard-assist parity"
