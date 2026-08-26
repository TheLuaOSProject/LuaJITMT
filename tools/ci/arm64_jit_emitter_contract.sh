#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_jit_emitter_contract SKIP: requires native macOS arm64"
  exit 0
fi

cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
archive=$root/src/libluajit.a
asm_object=$root/src/lj_asm.o
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-jit-emitter.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture=$tmpdir/t-arm64-jit-emitter
words=$tmpdir/emitted.bin
emitted_object=$tmpdir/emitted.o
empty_object=$tmpdir/empty.o
disasm=$tmpdir/emitted.disasm
region=$tmpdir/emitter-region.txt
fixed_region=$tmpdir/fixed-register-region.txt
archive_object=$tmpdir/lj_asm.archive.o
xcflags='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_ARM64_EMIT_TEST_HELPERS'

test -f "$archive" && test -f "$asm_object" || {
  echo "ARM64 JIT emitter contract requires an existing experimental build" >&2
  exit 1
}
ar -p "$archive" lj_asm.o >"$archive_object"
cmp "$asm_object" "$archive_object"
if ! nm "$asm_object" | grep ' T _lj_asm_arm64_emit_test$' >/dev/null; then
  echo "experimental assembler object lacks the TG emitter test surface" >&2
  exit 1
fi

awk '
  /-- TG-local JIT state / { copying = 1 }
  copying { print }
  /-- End TG-local JIT state / { exit }
' "$root/src/lj_emit_arm64.h" >"$region"
test -s "$region"
for required in RID_DISPATCH A64I_LDARx A64I_STLRx A64I_DMB_ISH A64I_STRw; do
  grep "$required" "$region" >/dev/null || {
    echo "ARM64 TG emitter is missing $required" >&2
    exit 1
  }
done
if grep -E 'RID_GL|J2G|global_State|emit_(get|set)gl|glofs' "$region" >/dev/null; then
  echo "ARM64 TG emitter contains a global-state approximation" >&2
  exit 1
fi
awk '
  /#define RSET_FIXED/ { copying = 1 }
  copying { print }
  /#define RSET_GPR/ { exit }
' "$root/src/lj_target_arm64.h" >"$fixed_region"
if ! grep 'RID2RSET(RID_DISPATCH)' "$fixed_region" >/dev/null ||
   ! grep 'RID2RSET(RID_LR)' "$fixed_region" >/dev/null; then
  echo "ARM64 TG carrier or emitter scratch escaped RSET_FIXED" >&2
  exit 1
fi

# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  "$root/tests/t-arm64-jit-emitter.c" "$archive" -lm -pthread -o "$fixture"
"$fixture" "$words"

"$cc" -arch arm64 -mmacosx-version-min="$minver" -x assembler \
  -c /dev/null -o "$empty_object"
ld -r -arch arm64 -o "$emitted_object" "$empty_object" \
  -sectcreate __TEXT __text "$words"
otool -tvV "$emitted_object" >"$disasm"
for required in \
  'add[[:space:]]+x30, x25' \
  'ldar[[:space:]]+x0, \[x30\]' \
  'ldar[[:space:]]+x1, \[x30\]' \
  'stlr[[:space:]]+x2, \[x30\]' \
  'dmb[[:space:]]+ish' \
  'str[[:space:]]+w30, \[x25'; do
  grep -E "$required" "$disasm" >/dev/null || {
    echo "emitted ARM64 object is missing instruction: $required" >&2
    exit 1
  }
done
if grep -E '(^|[^[:alnum:]_])x22([^[:alnum:]_]|$)' "$disasm" >/dev/null; then
  echo "emitted ARM64 TG code references RID_GL/x22" >&2
  exit 1
fi

grep -E '[[:space:]](add[[:space:]]+x30, x25|ldar[[:space:]]+x[01], \[x30\]|stlr[[:space:]]+x2, \[x30\]|dmb[[:space:]]+ish|str[[:space:]]+w30, \[x25)' "$disasm"
echo "arm64_jit_emitter_contract OK: x25 TG words and acquire/release forms verified"
