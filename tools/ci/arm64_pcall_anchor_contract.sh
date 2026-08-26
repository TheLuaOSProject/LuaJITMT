#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_pcall_anchor_contract SKIP: requires native macOS arm64"
  exit 0
fi

frame_header=$root/src/lj_frame.h
vm_source=$root/src/vm_arm64.dasc
err_source=$root/src/lj_err.c
record_source=$root/src/lj_ffrecord.c
vm_object=${LJ_ARM64_VM_OBJECT:-$root/src/lj_vm.o}
archive=${LJ_ARM64_ARCHIVE:-$root/src/libluajit.a}
source_only=${LJ_ARM64_PCALL_ANCHOR_SOURCE_ONLY:-0}

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-pcall-anchor.XXXXXX")
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
    END { exit(found_start && ended && state == npattern ? 0 : 1) }
  ' "$pattern_file" "$seq_file"; then
    echo "ARM64 pcall-anchor source lacks $seq_description" >&2
    exit 1
  fi
}

if ! grep -Fq '#if (LJ_TARGET_X64 || LJ_TARGET_ARM64) && LJ_FR2' \
    "$frame_header"; then
  echo "ARM64/FR2 does not enable the packed protected-frame invariant" >&2
  exit 1
fi
if ! grep -Fq '((uint32_t)((uint64_t)frame_ftsz(f) >> 32))' \
    "$frame_header" ||
   ! grep -Fq '((ptrdiff_t)((uint32_t)frame_ftsz(f) >> 3))' \
    "$frame_header" ||
   ! grep -Fq '((ptrdiff_t)((uint32_t)frame_ftsz(f) & ~(uint32_t)FRAME_TYPEP))' \
    "$frame_header"; then
  echo "packed protected-frame accessors do not isolate the low and high words" >&2
  exit 1
fi

require_source_sequence "$vm_source" \
  '[.]macro arm64_vm_pcall_anchor, frame, top, topw' '[.]endmacro' \
  'layout-relative acquire and upper-word pack macro' \
  'add ATMP, DISPATCH, #[(]DISPATCH_TG[(]root_anchor_top[)]&0xfff000[)]' \
  'add ATMP, ATMP, #[(]DISPATCH_TG[(]root_anchor_top[)]&0xfff[)]' \
  'ldar topw, [[]ATMP[]]' \
  'orr frame, frame, top, lsl #32'

require_source_sequence "$vm_source" '[.]ffunc pcall' '[.]ffunc xpcall' \
  'pcall low-word construction, checkpoint pack and flag-preserving branch' \
  'add PC, TMP0, #16[+]FRAME_PCALL' \
  'arm64_vm_pcall_anchor PC, TMP3, TMP3w' \
  'beq ->vm_call_dispatch'

require_source_sequence "$vm_source" '[.]ffunc xpcall' \
  '//-- Coroutine library' \
  'xpcall low-word construction, checkpoint pack and flag-preserving branch' \
  'cmn ITYPE, #-LJ_TFUNC' \
  'add PC, TMP0, #24[+]FRAME_PCALL' \
  'arm64_vm_pcall_anchor PC, TMP3, TMP3w' \
  'bne ->fff_fallback'

require_source_sequence "$vm_source" '[|]->vm_return:' '[|]->vm_leave_cp:' \
  'common low-32 protected delta reconstruction' \
  'ands CARG1, PC, #FRAME_TYPE' \
  'and RB, PC, #0xfffffff8' \
  'sub RB, BASE, RB'

require_source_sequence "$vm_source" '[|]->vm_call_tail:' \
  'b ->vm_call_dispatch' \
  'fallback tail-call low-32 protected delta reconstruction' \
  'ands TMP0, PC, #FRAME_TYPE' \
  'and TMP1, PC, #0xfffffff8' \
  'sub RB, BASE, TMP1'

if grep -Eq 'and (RB|TMP1), PC, #~FRAME_TYPEP' "$vm_source"; then
  echo "ARM64 VM retains a full-width protected-frame delta mask" >&2
  exit 1
fi
if test "$(grep -Ec 'arm64_vm_pcall_anchor PC, TMP3, TMP3w' "$vm_source")" \
    -ne 2; then
  echo "ARM64 VM must pack exactly the pcall and xpcall fast frames" >&2
  exit 1
fi
if ! grep -Fq '#if LJ_FRAME_PCALL_ROOT_ANCHOR' "$err_source" ||
   ! grep -Fq 'frame_pcall_root_top(frame)' "$err_source" ||
   ! grep -Fq '#if LJ_FRAME_PCALL_ROOT_ANCHOR' "$record_source" ||
   ! grep -Fq 'lj_tg_root_anchor_top_forjit' "$record_source"; then
  echo "protected unwind or recorded-frame runtime guard lost its shared gate" >&2
  exit 1
fi

if test "$source_only" = 1; then
  echo "arm64_pcall_anchor_contract OK: packed-frame source invariant present"
  exit 0
fi

if test ! -f "$vm_object" || test ! -f "$archive"; then
  echo "ARM64 pcall-anchor contract needs a completed VM object and archive" >&2
  exit 1
fi
if ! file "$vm_object" | grep -Eq 'Mach-O 64-bit object arm64' ||
   test "$(lipo -archs "$vm_object" 2>/dev/null || true)" != arm64 ||
   test "$(lipo -archs "$archive" 2>/dev/null || true)" != arm64; then
  echo "ARM64 pcall-anchor contract requires thin arm64 artifacts" >&2
  exit 1
fi

for input in \
  "$frame_header" \
  "$vm_source" \
  "$err_source" \
  "$record_source" \
  "$root/src/lj_tg.h" \
  "$root/src/lj_arch.h" \
  "$root/src/host/buildvm_arch.h" \
  "$root/dynasm/dasm_arm64.lua"
do
  if test -f "$input" && test "$input" -nt "$vm_object"; then
    echo "ARM64 pcall-anchor contract needs a VM object newer than $input" >&2
    exit 1
  fi
done
if test "$vm_object" -nt "$archive" ||
   ! ar -p "$archive" lj_vm.o | cmp - "$vm_object"; then
  echo "ARM64 archive does not contain the inspected current lj_vm.o" >&2
  exit 1
fi

offset_probe=$tmpdir/offset.c
offset_exe=$tmpdir/offset
{
  printf '%s\n' '#include <stddef.h>'
  printf '%s\n' '#include <stdint.h>'
  printf '%s\n' '#include <stdio.h>'
  printf '%s\n' '#include "lj_tg.h"'
  printf '%s\n' 'int main(void) {'
  printf '%s\n' '  ptrdiff_t o = (ptrdiff_t)offsetof(TGState, root_anchor_top) -'
  printf '%s\n' '                (ptrdiff_t)offsetof(TGState, dispatch);'
  printf '%s\n' '  if (o < 0 || o > 0xffffff || (o & 3) != 0) return 2;'
  printf '%s\n' '  printf("%tx %tx %tx\n", (o & 0xfff000) >> 12, o & 0xfff, o);'
  printf '%s\n' '  return 0;'
  printf '%s\n' '}'
} >"$offset_probe"
cc=${CC:-cc}
if ! "$cc" -std=gnu11 -DLUAJIT_MT_ARM64_BOOTSTRAP \
    -DLUAJIT_DISABLE_JIT -I"$root/src" "$offset_probe" -o "$offset_exe"; then
  echo "ARM64 pcall-anchor contract could not resolve the TG offset" >&2
  exit 1
fi
set -- $("$offset_exe")
if test "$#" -ne 3; then
  echo "ARM64 pcall-anchor offset probe returned malformed output" >&2
  exit 1
fi
root_hi=$1
root_lo=$2

nm_text=$tmpdir/vm.nm
disasm=$tmpdir/vm.disasm
nm -n "$vm_object" >"$nm_text"
otool -tvV "$vm_object" >"$disasm"

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
  if ! extract_symbol_disasm "$object_symbol" "$object_range" ||
     ! awk '
       FNR == NR { pattern[++npattern] = $0; next }
       state < npattern && $0 ~ pattern[state+1] { state++ }
       END { exit(state == npattern ? 0 : 1) }
     ' "$pattern_file" "$object_range"; then
    echo "ARM64 $object_symbol lacks $object_description" >&2
    exit 1
  fi
  if grep -Eq '[[:space:]]bl[[:space:]]' "$object_range" &&
     { test "$object_symbol" = lj_ff_pcall ||
       test "$object_symbol" = lj_ff_xpcall; }; then
    echo "ARM64 $object_symbol checkpoint setup unexpectedly calls a helper" >&2
    exit 1
  fi
}

require_object_sequence lj_ff_pcall \
  'large-offset acquire pack before its flag-preserving dispatch branch' \
  'add[[:space:]]+x21, x8, #0x16' \
  "add[[:space:]]+x14, x25, #0x0*$root_hi, lsl #12" \
  "add[[:space:]]+x14, x14, #0x0*$root_lo([[:space:]]|$)" \
  'ldar[[:space:]]+w11, [[]x14[]]' \
  'orr[[:space:]]+x21, x21, x11, lsl #32' \
  'b[.]eq[[:space:]]+_lj_vm_call_dispatch'

require_object_sequence lj_ff_xpcall \
  'large-offset acquire pack before traceback validation dispatch' \
  'cmn[[:space:]]+x15, #0x9' \
  'add[[:space:]]+x21, x8, #0x1e' \
  "add[[:space:]]+x14, x25, #0x0*$root_hi, lsl #12" \
  "add[[:space:]]+x14, x14, #0x0*$root_lo([[:space:]]|$)" \
  'ldar[[:space:]]+w11, [[]x14[]]' \
  'orr[[:space:]]+x21, x21, x11, lsl #32' \
  'b[.]ne[[:space:]]+_lj_fff_fallback'

require_object_sequence lj_vm_return \
  'low-32 delta mask without truncating the return-PC register' \
  'ands[[:space:]]+x0, x21, #0x3' \
  'and[[:space:]]+x17, x21, #0xfffffff8' \
  'sub[[:space:]]+x17, x19, x17'

require_object_sequence lj_vm_call_tail \
  'low-32 fallback tail-call delta reconstruction' \
  'ands[[:space:]]+x8, x21, #0x3' \
  'and[[:space:]]+x9, x21, #0xfffffff8' \
  'sub[[:space:]]+x17, x19, x9'

if nm -u "$vm_object" "$archive" | grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "ARM64 pcall-anchor artifacts import an atomic runtime helper" >&2
  exit 1
fi

echo "arm64_pcall_anchor_contract OK: packed checkpoints and low-word consumers present"
