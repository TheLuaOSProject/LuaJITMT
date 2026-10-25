#!/bin/sh
set -eu
root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
. "$root/tools/ci/lib/arm64_profile.sh"
if lj_ci_native_arm64_or_skip arm64_jit_phi_spill_contract; then :; else
  status=$?; test "$status" -eq 1 && exit 0; exit "$status"
fi

lj_ci_profile_init "$root" arm64_jit_phi_spill_contract
lj_ci_tmpdir lj-arm64-jit-phi-spill
tmpdir=$LJ_CI_TMPDIR
arch=${LJ_ARM64_PHI_ARCH:-arm64}
case "$arch" in
  arm64) target_flags='-arch arm64' ;;
  arm64e) target_flags='-arch arm64e -mbranch-protection=bti' ;;
  *) echo "unsupported PHI spill target: $arch" >&2; exit 2 ;;
esac
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_ARM64_EMIT_TEST_HELPERS'
lj_ci_set_restore '-arch arm64' \
  '-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'
lj_ci_build_profile "$arch" "$target_flags" "$xcflags" phi-spill
lj_ci_require_symbols lj_asm_arm64_phi_spill_test
fixture=$tmpdir/t-arm64-jit-phi-spill
lj_ci_compile_fixture "$fixture" \
  "$root/tests/t-arm64-jit-phi-spill.c" \
  "$root/tests/t-arm64-jit-phi-spill-call.S"
lj_ci_run_fixture 30 "$fixture" "$tmpdir/emitted.bin"
lj_ci_compile_object "$tmpdir/empty.o" /dev/null \
  -Wno-unused-command-line-argument -x assembler
ld -r -arch "$arch" -o "$tmpdir/emitted.o" "$tmpdir/empty.o" \
  -sectcreate __TEXT __text "$tmpdir/emitted.bin"
otool -tvV "$tmpdir/emitted.o" >"$tmpdir/emitted.disasm"
# Ordinary same-block loads must still pair; the PHI-boundary stores must not.
grep -E '[[:space:]]ldp[[:space:]]+d' "$tmpdir/emitted.disasm" >/dev/null
if grep -E '[[:space:]]stp[[:space:]]+d' "$tmpdir/emitted.disasm" >/dev/null; then
  echo "ARM64 PHI copy still paired across the body entry" >&2
  exit 1
fi
echo "arm64_jit_phi_spill_contract OK: $arch production loop/PHI boundary executes under profile $LJ_CI_PROFILE_ID"
