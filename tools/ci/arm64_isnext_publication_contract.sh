#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_isnext_publication_contract SKIP: requires native macOS arm64"
  exit 0
fi

vm_source=$root/src/vm_arm64.dasc
vm_object=${LJ_ARM64_VM_OBJECT:-$root/src/lj_vm.o}
dispatch_object=${LJ_ARM64_DISPATCH_OBJECT:-$root/src/lj_dispatch.o}
archive=${LJ_ARM64_ARCHIVE:-$root/src/libluajit.a}
source_only=${LJ_ARM64_ISNEXT_SOURCE_ONLY:-0}

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-isnext.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
active_source=$tmpdir/isnext.nojit.dasc
jit_source=$tmpdir/isnext.jit.dasc

# Extract one DynASM JIT arm from BC_ISNEXT. Keeping both projections lets the
# bootstrap gate own its JIT-off transaction while also proving this slice did
# not silently delete the still-deferred JLOOP recovery path.
extract_isnext_arm() {
  want_jit=$1
  awk -v want_jit="$want_jit" '
  function if_jit(line) {
    return line ~ /^[[:space:]]*[|][.]if[[:space:]]+JIT([[:space:]]|$)/
  }
  function is_else(line) {
    return line ~ /^[[:space:]]*[|][.]else([[:space:]]|$)/
  }
  function is_endif(line) {
    return line ~ /^[[:space:]]*[|][.]endif([[:space:]]|$)/
  }
  /case BC_ISNEXT:/ && !inside {
    inside = 1
    active = 1
    found = 1
  }
  inside && /case BC_VARG:/ {
    ended = 1
    exit
  }
  inside {
    if (if_jit($0)) {
      depth++
      parent[depth] = active
      condition[depth] = want_jit
      active = parent[depth] && condition[depth]
      next
    }
    if (is_else($0)) {
      if (depth == 0) {
        bad = 1
        next
      }
      active = parent[depth] && !condition[depth]
      next
    }
    if (is_endif($0)) {
      if (depth == 0) {
        bad = 1
        next
      }
      active = parent[depth]
      delete parent[depth]
      delete condition[depth]
      depth--
      next
    }
    if (active)
      print
  }
  END {
    if (!found || !ended || depth != 0 || bad)
      exit 2
  }
' "$vm_source"
}

if ! extract_isnext_arm 0 >"$active_source"; then
  echo "ARM64 ISNEXT contract could not isolate the JIT-disabled source arm" >&2
  exit 1
fi
if ! extract_isnext_arm 1 >"$jit_source"; then
  echo "ARM64 ISNEXT contract could not isolate the JIT-enabled source arm" >&2
  exit 1
fi

require_fixed_register() {
  name=$1
  reg=$2
  if ! grep -Eq \
    "^[[:space:]]*[|][.]define[[:space:]]+$name,[[:space:]]+$reg([[:space:]]|$)" \
    "$vm_source"; then
    echo "ARM64 ISNEXT contract requires $name to remain $reg across C calls" >&2
    exit 1
  fi
}

require_fixed_register BASE x19
require_fixed_register PC x21
require_fixed_register LREG x23
require_fixed_register RA x27
require_fixed_register RC x28

# A plain VM bytecode fetch is sufficient here because the complete published
# guard word carries RD and the following target load has an architectural
# address dependency on that RD value. The guard store is release-published;
# Arm preserves address dependencies between Normal-memory accesses. Pin both
# halves of that chain so a future decode refactor cannot turn this into a mere
# control dependency without making the contract fail.
if ! awk '
  /[.]macro ins_NEXT/ && !inside { inside = 1; next }
  inside && /[.]endmacro/ { ended = 1; exit(state == 3 ? 0 : 1) }
  inside && state == 0 &&
    /ldr[[:space:]]+INS[ ]*w,[[:space:]]*\[PC\],[[:space:]]*#4/ {
      state = 1
      next
    }
  inside && state == 1 && /decode_RD[[:space:]]+RC,[[:space:]]*INS/ {
    state = 2
    next
  }
  inside && state == 2 && /br_auth[[:space:]]+TMP0/ { state = 3 }
  END { if (!inside || !ended || state != 3) exit 1 }
' "$vm_source"; then
  echo "ARM64 dispatch lost the full-guard-word to RD decode dependency" >&2
  exit 1
fi
if ! awk '
  /case BC_JMP:/ && !inside { inside = 1; next }
  inside && /break;/ { ended = 1; exit(state == 3 ? 0 : 1) }
  inside && state == 0 &&
    /add[[:space:]]+RC,[[:space:]]*PC,[[:space:]]*RC,[[:space:]]*lsl[[:space:]]*#2/ {
      state = 1
      next
    }
  inside && state == 1 &&
    /sub[[:space:]]+PC,[[:space:]]*RC,[[:space:]]*#0x20000/ {
      state = 2
      next
    }
  inside && state == 2 && /ins_next/ { state = 3 }
  END { if (!inside || !ended || state != 3) exit 1 }
' "$vm_source"; then
  echo "ARM64 BC_JMP lost the guard-RD address dependency for its target fetch" >&2
  exit 1
fi

# Require the complete target-first transaction. RA/RC are the two
# callee-saved bytecode addresses; every C argument register is rebuilt after
# the first helper call.
if ! awk '
  /bl extern lj_bc_publish_op_vm/ { calls++ }
  state == 0 && /sub[[:space:]]+RA,[[:space:]]*PC,[[:space:]]*#4/ {
    state = 1
    next
  }
  state == 1 && /str[[:space:]]+BASE,[[:space:]]*L->base/ {
    state = 2
    next
  }
  state == 2 && /str[[:space:]]+PC,[[:space:]]*SAVE_PC/ {
    state = 3
    next
  }
  state == 3 && /mov[[:space:]]+CARG1,[[:space:]]*RC/ {
    state = 4
    next
  }
  state == 4 && /mov[[:space:]]+CARG2w,[[:space:]]*#BC_ITERC/ {
    state = 5
    next
  }
  state == 5 && /bl extern lj_bc_publish_op_vm/ {
    state = 6
    next
  }
  state == 6 && /mov[[:space:]]+CARG1,[[:space:]]*RA/ {
    state = 7
    next
  }
  state == 7 && /mov[[:space:]]+CARG2w,[[:space:]]*#BC_JMP/ {
    state = 8
    next
  }
  state == 8 && /bl extern lj_bc_publish_op_vm/ {
    state = 9
    next
  }
  state == 9 && /ldr[[:space:]]+BASE,[[:space:]]*L->base/ {
    state = 10
    next
  }
  state == 10 && /b[[:space:]]+<1/ { state = 11 }
  END { exit(state == 11 && calls == 2 ? 0 : 1) }
' "$active_source"; then
  echo "ARM64 JIT-off ISNEXT lacks its complete target-first C-call transaction" >&2
  exit 1
fi

if grep -Eq \
  '^[[:space:]]*[|][[:space:]]+(strb|sturb)[[:space:]]' \
  "$active_source"; then
  echo "ARM64 JIT-off ISNEXT still contains an opcode-byte store" >&2
  exit 1
fi

# Preserve the JIT-enabled implementation until the ARM64 JIT itself is
# ported. This is deliberately a preservation contract, not approval of its
# legacy opcode-byte stores: it must retain target inspection, JLOOP recovery
# from the trace start instruction and redispatch.
if ! awk '
  /bl extern lj_bc_publish_op_vm/ { calls++ }
  state == 0 && /ldrb[[:space:]]+TMP2w,[[:space:]]*\[RC,[[:space:]]*# OFS_OP\]/ {
    state = 1
    next
  }
  state == 1 && /strb[[:space:]]+TMP0w,[[:space:]]*\[PC,[[:space:]]*#-4[+]OFS_OP\]/ {
    state = 2
    next
  }
  state == 2 && /cmp[[:space:]]+TMP2w,[[:space:]]*#BC_ITERN/ {
    state = 3
    next
  }
  state == 3 && /strb[[:space:]]+TMP1w,[[:space:]]*\[RC,[[:space:]]*# OFS_OP\]/ {
    state = 4
    next
  }
  state == 4 && /^[[:space:]]*[|]6:/ { state = 5; next }
  state == 5 && /ldr[[:space:]]+TRACE:RA,[[:space:]]*\[RA,[[:space:]]*TMP2,[[:space:]]*lsl[[:space:]]*#3\]/ {
    state = 6
    next
  }
  state == 6 && /ldr[[:space:]]+TMP2w,[[:space:]]*TRACE:RA->startins/ {
    state = 7
    next
  }
  state == 7 && /str[[:space:]]+TMP2w,[[:space:]]*\[RC\]/ {
    state = 8
    next
  }
  state == 8 && /b[[:space:]]+<1/ { state = 9 }
  END { exit(state == 9 && calls == 0 ? 0 : 1) }
' "$jit_source"; then
  echo "ARM64 ISNEXT JIT arm no longer preserves its deferred JLOOP recovery" >&2
  exit 1
fi

if test "$source_only" = 1; then
  echo "arm64_isnext_publication_contract OK: source transaction and dependency present"
  exit 0
fi

for object in "$vm_object" "$dispatch_object"; do
  if test ! -f "$object"; then
    echo "ARM64 ISNEXT contract needs a completed bootstrap object: $object" >&2
    exit 1
  fi
  if ! file "$object" | grep -Eq 'Mach-O 64-bit object arm64'; then
    echo "ARM64 ISNEXT contract received a non-arm64 object: $object" >&2
    exit 1
  fi
  if test "$(lipo -archs "$object" 2>/dev/null || true)" != arm64; then
    echo "ARM64 ISNEXT contract requires a thin arm64 object: $object" >&2
    exit 1
  fi
done

if test ! -f "$archive"; then
  echo "ARM64 ISNEXT contract needs the runtime archive: $archive" >&2
  exit 1
fi
if test "$(lipo -archs "$archive" 2>/dev/null || true)" != arm64; then
  echo "ARM64 ISNEXT contract requires a thin arm64 runtime archive" >&2
  exit 1
fi

for input in \
  "$vm_source" \
  "$root/src/lj_bc.h" \
  "$root/src/lj_obj.h" \
  "$root/src/lj_arch.h" \
  "$root/src/lj_def.h" \
  "$root/src/Makefile" \
  "$root/src/host/buildvm.c" \
  "$root/src/host/buildvm_asm.c" \
  "$root/src/host/buildvm_arch.h" \
  "$root/dynasm/dasm_proto.h" \
  "$root/dynasm/dasm_arm64.lua"
do
  if test -f "$input" && test "$input" -nt "$vm_object"; then
    echo "ARM64 ISNEXT contract needs a VM object newer than $input" >&2
    exit 1
  fi
done
if test "$root/src/lj_dispatch.c" -nt "$dispatch_object" ||
   test "$root/src/lj_bc.h" -nt "$dispatch_object" ||
   test "$root/src/lj_atomic.h" -nt "$dispatch_object"; then
  echo "ARM64 ISNEXT contract needs a dispatch object newer than its sources" >&2
  exit 1
fi

# The runtime fixture links this archive, while the disassembly checks below
# inspect the loose objects. Require byte-for-byte member identity so a stale
# archive cannot pair a good object contract with an old guard-first VM.
if ! ar -p "$archive" lj_vm.o | cmp - "$vm_object"; then
  echo "ARM64 runtime archive does not contain the inspected lj_vm.o" >&2
  exit 1
fi
if ! ar -p "$archive" lj_dispatch.o | cmp - "$dispatch_object"; then
  echo "ARM64 runtime archive does not contain the inspected lj_dispatch.o" >&2
  exit 1
fi

nm_text=$tmpdir/vm.nm
relocs=$tmpdir/vm.relocs
vm_disasm=$tmpdir/vm.disasm
dispatch_disasm=$tmpdir/dispatch.disasm
nm -n "$vm_object" >"$nm_text"
otool -rv "$vm_object" >"$relocs"
otool -tvV "$vm_object" >"$vm_disasm"
otool -tvV "$dispatch_object" >"$dispatch_disasm"

reject_symbol_instruction() {
  disasm=$1
  symbol=$2
  pattern=$3
  description=$4
  if ! awk -v label="_$symbol:" -v pattern="$pattern" '
    $0 == label { inside = 1; next }
    inside && /^_/ { exit(bad ? 1 : 0) }
    inside && $0 ~ pattern { bad = 1 }
    END { if (!inside || bad) exit 1 }
  ' "$disasm"; then
    echo "ARM64 $symbol contains $description" >&2
    exit 1
  fi
}

call_addrs_unsorted=$tmpdir/isnext.calls.unsorted
call_addrs=$tmpdir/isnext.calls
if ! awk -v symbol=_lj_BC_ISNEXT -v target='_lj_bc_publish_op_vm$' '
  function normhex(s, z) {
    z = "0000000000000000" s
    return substr(z, length(z)-15)
  }
  FNR == NR {
    if ($1 ~ /^[[:xdigit:]]+$/ && $2 ~ /^[Tt]$/) {
      addr = normhex($1)
      if ($3 == symbol) {
        start = addr
        found_symbol = 1
      } else if (found_symbol && end == "" && ("x" addr) > ("x" start)) {
        end = addr
      }
    }
    next
  }
  /^Relocation information \(__TEXT,__text\)/ { in_text = 1; next }
  /^Relocation information / { in_text = 0; next }
  in_text && $1 ~ /^[[:xdigit:]]+$/ {
    addr = normhex($1)
    if (found_symbol && end != "" &&
        ("x" addr) >= ("x" start) && ("x" addr) < ("x" end) &&
        $NF ~ target) {
      count++
      if ($5 != "BR26") bad_type = 1
      print addr
    }
  }
  END {
    exit(found_symbol && end != "" && count == 2 && !bad_type ? 0 : 1)
  }
' "$nm_text" "$relocs" >"$call_addrs_unsorted"; then
  echo "ARM64 lj_BC_ISNEXT lacks exactly two in-bounds BR26 publication calls" >&2
  exit 1
fi
LC_ALL=C sort "$call_addrs_unsorted" >"$call_addrs"

# Resolve the current enum values rather than baking mutable bytecode numbers
# into the Mach-O disassembly contract.
opcode_probe=$tmpdir/isnext-opcodes.c
opcode_exe=$tmpdir/isnext-opcodes
{
  printf '%s\n' '#include <stdio.h>'
  printf '%s\n' '#include "lj_bc.h"'
  printf '%s\n' 'int main(void) {'
  printf '%s\n' '  printf("%u %u\n", (unsigned)BC_ITERC, (unsigned)BC_JMP);'
  printf '%s\n' '  return 0;'
  printf '%s\n' '}'
} >"$opcode_probe"
cc=${CC:-cc}
if ! $cc -std=gnu11 -DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT \
       -I"$root/src" "$opcode_probe" -o "$opcode_exe"; then
  echo "ARM64 ISNEXT contract could not resolve bytecode opcode values" >&2
  exit 1
fi
opcode_values=$("$opcode_exe")
set -- $opcode_values
if test "$#" -ne 2; then
  echo "ARM64 ISNEXT opcode probe returned malformed output" >&2
  exit 1
fi
iterc_hex=$(printf '%x' "$1")
jmp_hex=$(printf '%x' "$2")

# Bind the ordered relocations to their exact AAPCS64 argument setup in the
# emitted symbol. This rules out a newer-but-wrong object with two unrelated or
# reversed calls, which the source check alone cannot establish.
if ! awk -v label=_lj_BC_ISNEXT: -v iterc="$iterc_hex" -v jmp="$jmp_hex" '
  function normhex(s, z) {
    z = "0000000000000000" s
    return substr(z, length(z)-15)
  }
  FNR == NR { call[++nc] = $1; next }
  $0 == label { inside = 1; found_symbol = 1; next }
  inside && /^_/ { inside = 0; ended = 1 }
  inside && $1 ~ /^[[:xdigit:]]+$/ {
    addr = normhex($1)
    if (state == 0 &&
        $0 ~ /sub[[:space:]]+x27,[[:space:]]*x21,[[:space:]]*#0x4/) {
      state = 1
    } else if (state == 1 &&
               $0 ~ /str[[:space:]]+x19,[[:space:]]*\[x23,[[:space:]]*#0x[[:xdigit:]]+\]/) {
      state = 2
    } else if (state == 2 &&
               $0 ~ /str[[:space:]]+x21,[[:space:]]*\[sp,[[:space:]]*#0x8\]/) {
      state = 3
    } else if (state == 3 && $0 ~ /mov[[:space:]]+x0,[[:space:]]*x28/) {
      state = 4
    } else if (state == 4 &&
               $0 ~ ("mov[[:space:]]+w1,[[:space:]]*#0x0*" iterc "([[:space:]]|$)")) {
      state = 5
    } else if (state == 5 && addr == call[1] &&
               $0 ~ /bl[[:space:]]+0x[[:xdigit:]]+/) {
      state = 6
    } else if (state == 6 && $0 ~ /mov[[:space:]]+x0,[[:space:]]*x27/) {
      state = 7
    } else if (state == 7 &&
               $0 ~ ("mov[[:space:]]+w1,[[:space:]]*#0x0*" jmp "([[:space:]]|$)")) {
      state = 8
    } else if (state == 8 && addr == call[2] &&
               $0 ~ /bl[[:space:]]+0x[[:xdigit:]]+/) {
      state = 9
    } else if (state == 9 &&
               $0 ~ /ldr[[:space:]]+x19,[[:space:]]*\[x23,[[:space:]]*#0x[[:xdigit:]]+\]/) {
      state = 10
    } else if (state == 10 && $0 ~ /b[[:space:]]+0x[[:xdigit:]]+/) {
      state = 11
    }
  }
  END {
    exit(found_symbol && ended && nc == 2 && state == 11 ? 0 : 1)
  }
' "$call_addrs" "$vm_disasm"; then
  echo "ARM64 lj_BC_ISNEXT emitted publication call sequence is not exact" >&2
  exit 1
fi

reject_symbol_instruction "$vm_disasm" lj_BC_ISNEXT \
  '[[:space:]](strb|sturb|strh|sturh)[[:space:]]' \
  'a byte- or halfword-width store'

# Require the complete helper body, including the actual address and register
# flow. Merely finding one acquire and one release somewhere in the symbol is
# insufficient to prove a full-word opcode merge at pc.
if ! awk -v label=_lj_bc_publish_op_vm: '
  $0 == label { inside = 1; found_symbol = 1; next }
  inside && /^_/ { inside = 0; ended = 1 }
  inside && $1 ~ /^[[:xdigit:]]+$/ {
    if (state == 0 && $0 ~ /bti[[:space:]]+c([[:space:]]|$)/) {
      next
    } else if (state == 0 &&
               $0 ~ /(ldar|ldapr)[[:space:]]+w8,[[:space:]]*\[x0\]/) {
      state = 1
    } else if (state == 1 &&
               $0 ~ /bfxil[[:space:]]+w8,[[:space:]]*w1,[[:space:]]*#0,[[:space:]]*#8/) {
      state = 2
    } else if (state == 2 &&
               $0 ~ /stlr[[:space:]]+w8,[[:space:]]*\[x0\]/) {
      state = 3
    } else if (state == 3 && $0 ~ /ret([[:space:]]|$)/) {
      state = 4
    } else {
      bad = 1
    }
  }
  END {
    exit(found_symbol && ended && state == 4 && !bad ? 0 : 1)
  }
' "$dispatch_disasm"; then
  echo "ARM64 lj_bc_publish_op_vm is not an exact 32-bit acquire/merge/release helper" >&2
  exit 1
fi

if nm -u "$vm_object" "$dispatch_object" | grep -E \
     '(__atomic|libatomic)' >/dev/null; then
  echo "ARM64 ISNEXT publication imports an atomic runtime helper" >&2
  exit 1
fi

echo "arm64_isnext_publication_contract OK: target-first full-word publication present"
