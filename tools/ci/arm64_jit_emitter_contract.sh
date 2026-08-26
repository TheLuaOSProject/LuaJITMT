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
asm_arm64_globals=$tmpdir/asm-arm64-global-accesses.txt
xpoll_region=$tmpdir/asm-xpoll-region.txt
archive_object=$tmpdir/lj_asm.archive.o
audit_object=$tmpdir/lj_asm-audit.o
audit_relocs=$tmpdir/lj_asm-audit.relocs
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
for required in RID_DISPATCH A64I_LDARw A64I_LDARx A64I_STLRx \
  A64I_DMB_ISH A64I_STRw; do
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

if grep -En 'emit_(get|set)gl\([^;]*(cur_L|jit_base|vmstate)' \
     "$root/src/lj_asm_arm64.h" >"$asm_arm64_globals"; then
  echo "ARM64 assembler still routes per-executor state through global_State" >&2
  cat "$asm_arm64_globals" >&2
  exit 1
fi
if test "$(grep -Ec '^[[:space:]]*emit_(get|set)tg\(' \
     "$root/src/lj_asm_arm64.h")" -ne 5 ||
   test "$(grep -Fc 'emit_gettg(as, RID_TMP, cur_L);' \
     "$root/src/lj_asm_arm64.h")" -ne 2; then
  echo "ARM64 assembler TG callsite inventory changed" >&2
  exit 1
fi
for required in \
  'emit_settg(as, base, jit_base);' \
  'emit_gettg(as, (pbase & 31), jit_base);' \
  'emit_gettg(as, r, jit_base);'; do
  grep -F "$required" "$root/src/lj_asm_arm64.h" >/dev/null || {
    echo "ARM64 assembler is missing TG callsite: $required" >&2
    exit 1
  }
done
if test "$(grep -Ec '^[[:space:]]*emit_(get|set)gl' \
     "$root/src/lj_asm_arm64.h")" -ne 5; then
  echo "ARM64 assembler global-field allowlist changed" >&2
  exit 1
fi
for required in \
  'emit_setgl(as, tab, gc.grayagain);' \
  'emit_getgl(as, link, gc.grayagain);' \
  'emit_getgl(as, tmp2, gc.threshold);' \
  'emit_getgl(as, RID_TMP, gc.total);' \
  'emit_getgl32acq(as, gate, gc2.jit_phase_gate);'; do
  grep -F "$required" "$root/src/lj_asm_arm64.h" >/dev/null || {
    echo "ARM64 assembler global-field allowlist is missing: $required" >&2
    exit 1
  }
done

# IR_XPOLL must lower on ARM64 as a gate guard followed by two independent
# naturally sized TG loads. Source order is reverse runtime emission order.
awk '/^static void asm_xpoll\(ASMState \*as, IRIns \*ir\)/ { copying = 1 }
     copying { print }
     copying && /^}/ { exit }' "$root/src/lj_asm_arm64.h" >"$xpoll_region"
test -s "$xpoll_region"
line_of() { grep -n "$1" "$xpoll_region" | sed -n "${2:-1}p" | cut -d: -f1; }
poll_guard=$(line_of 'asm_guardcc(as, CC_NE)' 1)
poll_cmp=$(line_of 'A64I_CMPw, gate, RID_ZERO' 1)
poll_or=$(line_of 'A64I_ORRw, gate, gate, profile' 1)
profile_load=$(line_of 'emit_gettg32(as, profile, profile_request)' 1)
poll_load=$(line_of 'emit_gettg32(as, gate, poll)' 1)
gate_guard=$(line_of 'asm_guardcc(as, CC_EQ)' 1)
gate_cmp=$(line_of 'A64I_CMPw, gate, RID_ZERO' 2)
gate_load=$(line_of 'emit_getgl32acq(as, gate, gc2.jit_phase_gate)' 1)
test "$poll_guard" -lt "$poll_cmp" && test "$poll_cmp" -lt "$poll_or" &&
test "$poll_or" -lt "$profile_load" && test "$profile_load" -lt "$poll_load" &&
test "$poll_load" -lt "$gate_guard" && test "$gate_guard" -lt "$gate_cmp" &&
test "$gate_cmp" -lt "$gate_load"
if grep -E 'emit_gettg\(|LDRx|LDARx|uint64|qword' "$xpoll_region" >/dev/null; then
  echo "ARM64 XPOLL combines or widens its 32 bit TG publications" >&2
  exit 1
fi
grep -F '#if LJ_TARGET_X86ORX64 || LJ_TARGET_ARM64' \
  "$root/src/lj_asm.c" >/dev/null || {
  echo "generic assembler still compiles ARM64 XPOLL as a no-op" >&2
  exit 1
}

# Compile without inlining so the artifact exposes every selected TG helper
# call as a BR26 relocation. The expected totals include generic assembler,
# ARM64-specific assembler and test-only wrapper callsites.
# shellcheck disable=SC2086 # xcflags intentionally expands to arguments.
"$cc" -std=gnu11 -O0 -Wall -Wextra -Werror -arch arm64 \
  -mmacosx-version-min="$minver" $xcflags -I"$root/src" \
  -c "$root/src/lj_asm.c" -o "$audit_object"
otool -rv "$audit_object" >"$audit_relocs"
if test "$(grep -Ec '_emit_gettg_$' "$audit_relocs")" -ne 8 ||
   test "$(grep -Ec '_emit_settg_$' "$audit_relocs")" -ne 3; then
  echo "ARM64 assembler artifact has the wrong TG helper call inventory" >&2
  exit 1
fi
if test "$(grep -Ec '_emit_gettg32_$' "$audit_relocs")" -ne 4 ||
   test "$(grep -Ec '_emit_getgl32acq_$' "$audit_relocs")" -ne 2; then
  echo "ARM64 assembler artifact has the wrong XPOLL acquire-helper inventory" >&2
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
  'ldar[[:space:]]+w3, \[x30\]' \
  'ldar[[:space:]]+w4, \[x30\]' \
  'ldar[[:space:]]+w5, \[x30\]' \
  'dmb[[:space:]]+ish' \
  'str[[:space:]]+w30, \[x25'; do
  grep -E "$required" "$disasm" >/dev/null || {
    echo "emitted ARM64 object is missing instruction: $required" >&2
    exit 1
  }
done
if test "$(grep -Ec 'add[[:space:]]+x30, x22' "$disasm")" -ne 1 ||
   grep -E 'ldar[[:space:]]+w[345], \[x22' "$disasm" >/dev/null; then
  echo "emitted ARM64 XPOLL gate address/load shape changed" >&2
  exit 1
fi

grep -E '[[:space:]](add[[:space:]]+x30, x(22|25)|ldar[[:space:]]+[xw][0-5], \[x30\]|stlr[[:space:]]+x2, \[x30\]|dmb[[:space:]]+ish|str[[:space:]]+w30, \[x25)' "$disasm"
echo "arm64_jit_emitter_contract OK: TG state and 32 bit XPOLL acquire forms verified"
