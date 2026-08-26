#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_vm_safepoint_contract SKIP: requires native macOS arm64"
  exit 0
fi

vm_source=$root/src/vm_arm64.dasc
tg_header=$root/src/lj_tg.h
vm_object=${LJ_ARM64_VM_OBJECT:-$root/src/lj_vm.o}
archive=${LJ_ARM64_ARCHIVE:-$root/src/libluajit.a}
source_only=${LJ_ARM64_SAFEPOINT_SOURCE_ONLY:-0}

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-vm-safepoint.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
pattern_file=$tmpdir/patterns

require_source_sequence() {
  seq_file=$1
  seq_start=$2
  seq_stop=$3
  seq_description=$4
  shift 4
  : >"$pattern_file"
  for seq_pattern do
    printf '%s\n' "$seq_pattern" >>"$pattern_file"
  done
  if ! awk -v start="$seq_start" -v stop="$seq_stop" '
    FNR == NR { pattern[++npattern] = $0; next }
    $0 ~ start && !inside {
      inside = 1
      found_start = 1
      start_line = FNR
      next
    }
    inside && FNR != start_line && $0 ~ stop {
      ended = 1
      inside = 0
      next
    }
    inside && state < npattern && $0 ~ pattern[state+1] { state++ }
    END {
      exit(found_start && ended && state == npattern ? 0 : 1)
    }
  ' "$pattern_file" "$seq_file"; then
    echo "ARM64 safepoint source lacks $seq_description" >&2
    exit 1
  fi
}

reject_source_region() {
  reject_file=$1
  reject_start=$2
  reject_stop=$3
  reject_pattern=$4
  reject_description=$5
  if ! awk -v start="$reject_start" -v stop="$reject_stop" \
      -v pattern="$reject_pattern" '
    $0 ~ start && !inside {
      inside = 1
      found_start = 1
      start_line = FNR
      next
    }
    inside && FNR != start_line && $0 ~ stop {
      ended = 1
      inside = 0
      next
    }
    inside && $0 ~ pattern { bad = 1 }
    END { exit(found_start && ended && !bad ? 0 : 1) }
  ' "$reject_file"; then
    echo "ARM64 safepoint source contains forbidden $reject_description" >&2
    exit 1
  fi
}

if grep -F 'LJ_ARM64_JIT_NATIVE_ENTRY_FAIL_CLOSED' \
     "$vm_source" "$root/tests/t-vm-safepoint.c" >/dev/null; then
  echo "ARM64 VM safepoint coverage still uses the aggregate native-entry gate" >&2
  exit 1
fi
if grep -F 'LJ_ARM64_JIT_RECORDER_ADMISSION_FAIL_CLOSED' \
     "$root/tests/t-vm-safepoint.c" >/dev/null; then
  echo "ARM64 VM safepoint fixture still uses the aggregate recorder gate" >&2
  exit 1
fi
for gate in \
  LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED \
  LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED \
  LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED; do
  test "$(grep -Fc "#if $gate" "$vm_source")" -eq 1 || {
    echo "ARM64 VM must contain exactly one topology gate for $gate" >&2
    exit 1
  }
done
for gate in \
  LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED \
  LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED \
  LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED \
  LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED \
  LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED \
  LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED; do
  grep -F "$gate" "$root/tests/t-vm-safepoint.c" >/dev/null || {
    echo "ARM64 VM safepoint fixture lost granular gate $gate" >&2
    exit 1
  }
done

# The two publications are independent 32-bit words. Pin their natural-width
# acquire loads and the combined decision instead of accepting x64's adjacent
# qword shortcut on an architecture where that would not be an acquire load.
require_source_sequence "$vm_source" \
  '[.]macro arm64_vm_poll_acq, dst, tmp' '[.]endmacro' \
  'separate acquire poll/profile_request macro' \
  'add ATMP, DISPATCH, #DISPATCH_TG[(]poll[)]' \
  'ldar dst, [[]ATMP[]]' \
  'add ATMP, DISPATCH, #DISPATCH_TG[(]profile_request[)]' \
  'ldar tmp, [[]ATMP[]]' \
  'orr dst, dst, tmp'

# All ordinary dispatch edges share one frame-completing, checked helper.
require_source_sequence "$vm_source" '[|]->vm_safepoint:' \
  '[|]->vm_call_publish:' 'common checked safepoint frame sequence' \
  'str BASE, L->base' \
  'str PC, SAVE_PC' \
  'mov CARG1, L' \
  'bl extern lj_safepoint_ack_check' \
  'ldr BASE, L->base' \
  'ins_next'

# A return to C must acknowledge before retiring the current C frame. The
# helper may throw on fresh STOPREQ, so the success status is formed only on
# the no-throw continuation.
require_source_sequence "$vm_source" '[|]->vm_leave_cp:' \
  '[|]->vm_leave_unw:' 'C-leave checked poll before cframe retirement' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbz TMP0w, >9' \
  'str L, SAVE_PC' \
  'mov CARG1, L' \
  'bl extern lj_safepoint_ack_check' \
  '^[[:space:]]*[|]9:' \
  'ldr RC, SAVE_CFRAME' \
  'mov CRET1, #0' \
  'str RC, L->cframe'

# An external unwind must preserve the original protected-call status across
# acknowledgement. The checked helper is allowed to replace it only by
# throwing a fresh STOPREQ through the same protected boundary.
require_source_sequence "$vm_source" '[|]->vm_unwind_c_eh:' \
  '[|]->vm_unwind_ff:' 'unwind status save/check/restore sequence' \
  'load_DISPATCH L' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbz TMP0w, >9' \
  'str CRET1w, SAVE_ERRF' \
  'str L, SAVE_PC' \
  'mov CARG1, L' \
  'bl extern lj_safepoint_ack_check' \
  'ldr CRET1w, SAVE_ERRF'

# A native trace exit becomes quiescent before acknowledging either request.
# Both the successful redispatch and non-returning error path preserve their
# result across the checked call and reload relocatable interpreter state.
require_source_sequence "$vm_source" '[|]->vm_exit_interp:' \
  '[|]5:.*Recover the original instruction' \
  'normal trace-exit quiescence and checked poll sequence' \
  'str BASE, L->base' \
  'clear_tg_jit_base' \
  'mv_vmstate CARG4w, INTERP' \
  'st_vmstate CARG4w' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbz TMP0w, >7' \
  'str CARG1w, SAVE_ERRF' \
  'str PC, SAVE_PC' \
  'mov CARG1, L' \
  'bl extern lj_safepoint_ack_check' \
  'ldr L, SAVE_L' \
  'ldr BASE, L->base' \
  'ldr CARG1w, SAVE_ERRF'

require_source_sequence "$vm_source" '[|]9:.*Rethrow error' \
  '[|]->vm_modi:' 'error trace-exit quiescence and checked poll sequence' \
  'str BASE, L->base' \
  'clear_tg_jit_base' \
  'mv_vmstate CARG4w, INTERP' \
  'st_vmstate CARG4w' \
  'str CARG1w, SAVE_ERRF' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbz TMP0w, >8' \
  'str PC, SAVE_PC' \
  'mov CARG1, L' \
  'bl extern lj_safepoint_ack_check' \
  'ldr L, SAVE_L' \
  'ldr BASE, L->base' \
  'ldr CARG2w, SAVE_ERRF'

# Interpreter progress edges. Poll only after each edge has published the
# frame/control state which the owner scan will treat as authoritative.
require_source_sequence "$vm_source" 'case BC_ITERN:' 'case BC_ISNEXT:' \
  'positive ITERN target publication and common poll route' \
  'cmp RCw, #0' \
  'ble >3' \
  'sub PC, TMP3, #0x20000' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbnz TMP0w, ->vm_safepoint' \
  '^[[:space:]]*[|]3:' \
  'ins_next'

require_source_sequence "$vm_source" 'case BC_FORI:' 'case BC_ITERL:' \
  'taken integer and floating IFORL paths through one poll edge' \
  'if [(]op == BC_IFORL[)]' \
  'ble >8' \
  'bls >8' \
  '^[[:space:]]*[|]8:' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbnz TMP0w, ->vm_safepoint' \
  'b <2'

require_source_sequence "$vm_source" 'case BC_IITERL:' 'case BC_LOOP:' \
  'nonnull IITERL control publication and common poll route' \
  'beq >1' \
  'sub PC, TMP0, #0x20000' \
  'str CARG1, [[]TMP1, #-8[]]' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbnz TMP0w, ->vm_safepoint' \
  '^[[:space:]]*[|]1:' \
  'ins_next'

require_source_sequence "$vm_source" 'case BC_ILOOP:' 'case BC_JLOOP:' \
  'ILOOP progress poll' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbnz TMP0w, ->vm_safepoint' \
  'ins_next'

require_source_sequence "$vm_source" 'case BC_JMP:' 'Function headers' \
  'JMP target publication before its progress poll' \
  'add RC, PC, RC, lsl #2' \
  'sub PC, RC, #0x20000' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbnz TMP0w, ->vm_safepoint' \
  'ins_next'

require_source_sequence "$vm_source" 'case BC_IFUNCF:' 'case BC_JFUNCV:' \
  'ordinary fixed-argument entry poll after missing-parameter fill' \
  'cmp NARGS8:RC, TMP1, lsl #3' \
  'blo >3' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbnz TMP0w, ->vm_safepoint' \
  'ins_next' \
  '^[[:space:]]*[|]3:' \
  'str TISNIL, [[]BASE, NARGS8:RC[]]' \
  'b <2'

require_source_sequence "$vm_source" 'case BC_IFUNCV:' 'case BC_FUNCC:' \
  'ordinary vararg entry publication before its poll' \
  'arm64_vm_stack_dirty' \
  'mov BASE, KBASE' \
  'ldr KBASE, [[]PC, #-4[+]PC2PROTO[(]k[)][]]' \
  'arm64_vm_poll_acq TMP0w, TMP1w' \
  'cbnz TMP0w, ->vm_safepoint' \
  'ins_next'

# These I-label handlers are used by ordinary interpreted bytecode in both a
# JIT-capable build with a function disabled and a compile-time no-JIT build.
# A preprocessor guard here would make one of those builds lose progress.
reject_source_region "$vm_source" 'case BC_FORI:' 'case BC_ITERL:' \
  '^[[:space:]]*#[[:space:]]*if.*LJ_HASJIT' \
  'LJ_HASJIT guard around the ordinary IFORL poll'
reject_source_region "$vm_source" 'case BC_IFUNCF:' 'case BC_JFUNCV:' \
  '^[[:space:]]*#[[:space:]]*if.*LJ_HASJIT' \
  'LJ_HASJIT guard around the ordinary IFUNCF poll'
reject_source_region "$vm_source" 'case BC_IFUNCV:' 'case BC_FUNCC:' \
  '^[[:space:]]*#[[:space:]]*if.*LJ_HASJIT' \
  'LJ_HASJIT guard around the ordinary IFUNCV poll'

checked_calls=$(grep -Ec \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_safepoint_ack_check' \
  "$vm_source" || true)
if test "$checked_calls" -ne 5; then
  echo "ARM64 VM must have exactly five checked safepoint helper call sites" >&2
  exit 1
fi
if grep -Eq \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_safepoint_(ack|poll)([^_]|$)' \
  "$vm_source"; then
  echo "ARM64 VM contains a raw non-checking safepoint helper call" >&2
  exit 1
fi

if test "$source_only" = 1; then
  echo "arm64_vm_safepoint_contract OK: granular topology gates and source progress edges present"
  exit 0
fi

# The runtime fixture links the archive while the emitted-code checks inspect
# the loose object. Refuse stale, foreign-architecture or mismatched pairs.
if test ! -f "$vm_object"; then
  echo "ARM64 safepoint contract needs a completed VM object: $vm_object" >&2
  exit 1
fi
if test ! -f "$archive"; then
  echo "ARM64 safepoint contract needs a completed runtime archive: $archive" >&2
  exit 1
fi
if ! file "$vm_object" | grep -Eq 'Mach-O 64-bit object arm64'; then
  echo "ARM64 safepoint contract received a non-arm64 VM object" >&2
  exit 1
fi
if test "$(lipo -archs "$vm_object" 2>/dev/null || true)" != arm64; then
  echo "ARM64 safepoint contract requires a thin arm64 VM object" >&2
  exit 1
fi
if test "$(lipo -archs "$archive" 2>/dev/null || true)" != arm64; then
  echo "ARM64 safepoint contract requires a thin arm64 runtime archive" >&2
  exit 1
fi

for input in \
  "$vm_source" \
  "$tg_header" \
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
    echo "ARM64 safepoint contract needs a VM object newer than $input" >&2
    exit 1
  fi
done
if test "$vm_object" -nt "$archive"; then
  echo "ARM64 safepoint contract needs an archive newer than its VM object" >&2
  exit 1
fi
if ! ar -p "$archive" lj_vm.o | cmp - "$vm_object"; then
  echo "ARM64 safepoint archive does not contain the inspected lj_vm.o" >&2
  exit 1
fi

nm_text=$tmpdir/vm.nm
relocs=$tmpdir/vm.relocs
disasm=$tmpdir/vm.disasm
nm -n "$vm_object" >"$nm_text"
otool -rv "$vm_object" >"$relocs"
otool -tvV "$vm_object" >"$disasm"

require_symbol_relocation() {
  reloc_symbol=$1
  reloc_target=$2
  reloc_description=$3
  if ! awk -v symbol="_$reloc_symbol" -v target="$reloc_target" '
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
    /^Relocation information [(]__TEXT,__text[)]/ { in_text = 1; next }
    /^Relocation information / { in_text = 0; next }
    in_text && $1 ~ /^[[:xdigit:]]+$/ {
      addr = normhex($1)
      if (found_symbol && end != "" &&
          ("x" addr) >= ("x" start) && ("x" addr) < ("x" end) &&
          $NF ~ target) {
        found_reloc = 1
        if ($5 != "BR26") bad_type = 1
      }
    }
    END {
      exit(found_symbol && end != "" && found_reloc && !bad_type ? 0 : 1)
    }
  ' "$nm_text" "$relocs"; then
    echo "ARM64 $reloc_symbol lacks $reloc_description" >&2
    exit 1
  fi
}

reloc_count=$(awk '
  /^Relocation information [(]__TEXT,__text[)]/ { in_text = 1; next }
  /^Relocation information / { in_text = 0; next }
  in_text && $NF == "_lj_safepoint_ack_check" {
    count++
    if ($5 != "BR26") bad = 1
  }
  END {
    if (bad) exit 2
    print count + 0
  }
' "$relocs")
object_checked_calls=3
reloc_symbols='lj_vm_safepoint lj_vm_leave_cp lj_vm_unwind_c_eh'
if nm "$archive" | grep -E ' T _lj_trace_exit$' >/dev/null; then
  object_checked_calls=5
  reloc_symbols="$reloc_symbols lj_vm_exit_interp"
fi
if test "$reloc_count" -ne "$object_checked_calls"; then
  echo "ARM64 VM emitted the wrong checked safepoint call inventory" >&2
  exit 1
fi
for reloc_symbol in $reloc_symbols; do
  require_symbol_relocation "$reloc_symbol" '_lj_safepoint_ack_check$' \
    'an in-bounds checked safepoint helper relocation'
done
if grep -Eq '_lj_safepoint_(ack|poll)$' "$relocs"; then
  echo "ARM64 VM emits a raw non-checking safepoint helper relocation" >&2
  exit 1
fi

# Resolve the offsets used by DISPATCH_TG rather than baking the current
# TGState layout into the disassembly contract.
offset_probe=$tmpdir/safepoint-offsets.c
offset_exe=$tmpdir/safepoint-offsets
{
  printf '%s\n' '#include <stddef.h>'
  printf '%s\n' '#include <stdio.h>'
  printf '%s\n' '#include "lj_tg.h"'
  printf '%s\n' 'int main(void) {'
  printf '%s\n' '  ptrdiff_t base = (ptrdiff_t)offsetof(TGState, dispatch);'
  printf '%s\n' '  ptrdiff_t poll = (ptrdiff_t)offsetof(TGState, poll) - base;'
  printf '%s\n' '  ptrdiff_t profile = (ptrdiff_t)offsetof(TGState, profile_request) - base;'
  printf '%s\n' '  if (poll < 0 || profile < 0) return 2;'
  printf '%s\n' '  printf("%tx %tx\n", poll, profile);'
  printf '%s\n' '  return 0;'
  printf '%s\n' '}'
} >"$offset_probe"
cc=${CC:-cc}
if ! "$cc" -std=gnu11 -DLUAJIT_MT_ARM64_BOOTSTRAP \
    -DLUAJIT_DISABLE_JIT -I"$root/src" "$offset_probe" -o "$offset_exe"; then
  echo "ARM64 safepoint contract could not resolve TG dispatch offsets" >&2
  exit 1
fi
set -- $("$offset_exe")
if test "$#" -ne 2; then
  echo "ARM64 safepoint offset probe returned malformed output" >&2
  exit 1
fi
poll_hex=$1
profile_hex=$2

extract_symbol_disasm() {
  extract_symbol=$1
  extract_output=$2
  awk -v symbol="_$extract_symbol" '
    function normhex(s, z) {
      sub(/^0x/, "", s)
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
    $1 ~ /^[[:xdigit:]]+$/ {
      addr = normhex($1)
      if (found_symbol && end != "" &&
          ("x" addr) >= ("x" start) && ("x" addr) < ("x" end))
        print
    }
    END { if (!found_symbol || end == "") exit 2 }
  ' "$nm_text" "$disasm" >"$extract_output"
}

object_sequence_id=0
require_object_sequence() {
  object_symbol=$1
  object_description=$2
  shift 2
  object_sequence_id=$((object_sequence_id + 1))
  object_range=$tmpdir/object-range.$object_sequence_id
  : >"$pattern_file"
  for object_pattern do
    printf '%s\n' "$object_pattern" >>"$pattern_file"
  done
  if ! extract_symbol_disasm "$object_symbol" "$object_range"; then
    echo "ARM64 object lacks bounded symbol $object_symbol" >&2
    exit 1
  fi
  if ! awk '
    FNR == NR { pattern[++npattern] = $0; next }
    state < npattern && $0 ~ pattern[state+1] { state++ }
    END { exit(state == npattern ? 0 : 1) }
  ' "$pattern_file" "$object_range"; then
    echo "ARM64 $object_symbol lacks $object_description" >&2
    exit 1
  fi
}

require_poll_pair() {
  poll_symbol=$1
  require_object_sequence "$poll_symbol" \
    'the exact separate poll/profile acquire pair and common branch' \
    "add[[:space:]]+x14, x25, #0x0*$poll_hex([[:space:]]|$)" \
    'ldar[[:space:]]+w8, [[]x14[]]' \
    "add[[:space:]]+x14, x25, #0x0*$profile_hex([[:space:]]|$)" \
    'ldar[[:space:]]+w9, [[]x14[]]' \
    'orr[[:space:]]+w8, w8, w9' \
    'cbnz[[:space:]]+w8, _lj_vm_safepoint'
}

for poll_symbol in \
  lj_BC_ITERN \
  lj_BC_FORL \
  lj_BC_ITERL \
  lj_BC_LOOP \
  lj_BC_JMP \
  lj_BC_IFUNCF \
  lj_BC_IFUNCV
do
  require_poll_pair "$poll_symbol"
done

require_object_sequence lj_vm_safepoint \
  'frame save, checked call and BASE reload' \
  'str[[:space:]]+x19, [[]x23, #0x[[:xdigit:]]+[]]' \
  'str[[:space:]]+x21, [[]sp, #0x8[]]' \
  'mov[[:space:]]+x0, x23' \
  'bl[[:space:]]+0x[[:xdigit:]]+' \
  'ldr[[:space:]]+x19, [[]x23, #0x[[:xdigit:]]+[]]'

require_object_sequence lj_vm_leave_cp \
  'poll, checked call and post-call success/cframe sequence' \
  "add[[:space:]]+x14, x25, #0x0*$poll_hex([[:space:]]|$)" \
  'ldar[[:space:]]+w8, [[]x14[]]' \
  "add[[:space:]]+x14, x25, #0x0*$profile_hex([[:space:]]|$)" \
  'ldar[[:space:]]+w9, [[]x14[]]' \
  'orr[[:space:]]+w8, w8, w9' \
  'cbz[[:space:]]+w8,' \
  'str[[:space:]]+x23, [[]sp, #0x8[]]' \
  'mov[[:space:]]+x0, x23' \
  'bl[[:space:]]+0x[[:xdigit:]]+' \
  'ldr[[:space:]]+x28, [[]sp[]]' \
  'mov[[:space:]]+w0, #0x0' \
  'str[[:space:]]+x28, [[]x23, #0x[[:xdigit:]]+[]]'

require_object_sequence lj_vm_unwind_c_eh \
  'poll and exact protected status save/check/restore sequence' \
  "add[[:space:]]+x14, x25, #0x0*$poll_hex([[:space:]]|$)" \
  'ldar[[:space:]]+w8, [[]x14[]]' \
  "add[[:space:]]+x14, x25, #0x0*$profile_hex([[:space:]]|$)" \
  'ldar[[:space:]]+w9, [[]x14[]]' \
  'orr[[:space:]]+w8, w8, w9' \
  'cbz[[:space:]]+w8,' \
  'str[[:space:]]+w0, [[]sp, #0x24[]]' \
  'str[[:space:]]+x23, [[]sp, #0x8[]]' \
  'mov[[:space:]]+x0, x23' \
  'bl[[:space:]]+0x[[:xdigit:]]+' \
  'ldr[[:space:]]+w0, [[]sp, #0x24[]]'

if nm -u "$vm_object" "$archive" | grep -E \
     '(__atomic|libatomic)' >/dev/null; then
  echo "ARM64 safepoint object/archive imports an atomic runtime helper" >&2
  exit 1
fi

echo "arm64_vm_safepoint_contract OK: acquire polls, progress and checked exits present"
