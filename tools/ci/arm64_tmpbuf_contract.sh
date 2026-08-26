#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
vm_source=$root/src/vm_arm64.dasc
vm_object=${LJ_ARM64_VM_OBJECT:-$root/src/lj_vm.o}
archive=${LJ_ARM64_ARCHIVE:-$root/src/libluajit.a}
source_only=${LJ_ARM64_TMPBUF_SOURCE_ONLY:-0}

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-tmpbuf.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
pattern_file=$tmpdir/patterns

require_source_sequence() {
  seq_description=$1
  shift
  : >"$pattern_file"
  for seq_pattern do
    printf '%s\n' "$seq_pattern" >>"$pattern_file"
  done
  if ! awk '
    FNR == NR { pattern[++npattern] = $0; next }
    /[.]macro ffstring_op, name/ && !inside {
      inside = 1
      found_start = 1
      next
    }
    inside && /[.]endmacro/ {
      ended = 1
      inside = 0
      next
    }
    inside && state < npattern && $0 ~ pattern[state+1] { state++ }
    END { exit(found_start && ended && state == npattern ? 0 : 1) }
  ' "$pattern_file" "$vm_source"; then
    echo "ARM64 tmpbuf source lacks $seq_description" >&2
    exit 1
  fi
}

if ! grep -Fq 'LJ_STATIC_ASSERT(DISPATCH_TG(tmpbuf) >= 0 &&' "$vm_source" ||
   ! grep -Fq 'DISPATCH_TG(tmpbuf) <= 0xffffff);' "$vm_source" ||
   ! grep -Fq 'LJ_STATIC_ASSERT(offsetof(SBuf, w) == 0);' "$vm_source" ||
   ! grep -Fq '(offsetof(SBuf, b) & 7) == 0);' "$vm_source" ||
   ! grep -Fq '(offsetof(SBuf, L) & 7) == 0);' "$vm_source"; then
  echo "ARM64 tmpbuf source lacks its offset and SBuf layout assertions" >&2
  exit 1
fi

require_source_sequence \
  'split x25 TG address and weak-order SBuf reset' \
  'add ATMP, DISPATCH, #[(]DISPATCH_TG[(]tmpbuf[)]&0xfff000[)]' \
  'add SBUF:CARG1, ATMP, #[(]DISPATCH_TG[(]tmpbuf[)]&0xfff[)]' \
  'add ATMP, CARG1, #offsetof[(]SBuf, b[)]' \
  'ldar TMP0, [[]ATMP[]]' \
  'str L, SBUF:CARG1->L' \
  'stlr TMP0, [[]CARG1[]]'

if awk '
  /[.]macro ffstring_op, name/ { inside = 1; next }
  inside && /[.]endmacro/ { exit bad ? 0 : 1 }
  inside && (/GL->tmpbuf/ || /offsetof[(]global_State, tmpbuf[)]/ ||
             /DISPATCH, #DISPATCH_TG[(]tmpbuf[)]/) { bad = 1 }
  END { if (inside) exit bad ? 0 : 1 }
' "$vm_source"; then
  echo "ARM64 ffstring_op retains global or single-immediate tmpbuf addressing" >&2
  exit 1
fi

if test "$source_only" = 1; then
  echo "arm64_tmpbuf_contract OK: split TG source reset present"
  exit 0
fi

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_tmpbuf_contract SKIP: object check requires native macOS arm64"
  exit 0
fi

if test ! -f "$vm_object" || test ! -f "$archive"; then
  echo "ARM64 tmpbuf contract needs a completed VM object and archive" >&2
  exit 1
fi
if ! file "$vm_object" | grep -Eq 'Mach-O 64-bit object arm64' ||
   test "$(lipo -archs "$vm_object" 2>/dev/null || true)" != arm64 ||
   test "$(lipo -archs "$archive" 2>/dev/null || true)" != arm64; then
  echo "ARM64 tmpbuf contract requires thin arm64 artifacts" >&2
  exit 1
fi

for input in \
  "$vm_source" \
  "$root/src/lj_obj.h" \
  "$root/src/lj_tg.h" \
  "$root/src/lj_arch.h" \
  "$root/src/host/buildvm_arch.h" \
  "$root/dynasm/dasm_arm64.lua"
do
  if test -f "$input" && test "$input" -nt "$vm_object"; then
    echo "ARM64 tmpbuf contract needs a VM object newer than $input" >&2
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
  printf '%s\n' '  ptrdiff_t o = (ptrdiff_t)offsetof(TGState, tmpbuf) -'
  printf '%s\n' '                (ptrdiff_t)offsetof(TGState, dispatch);'
  printf '%s\n' '  if (o < 0 || o > 0xffffff || offsetof(SBuf, w) != 0 ||'
  printf '%s\n' '      offsetof(SBuf, b) > 4095 || offsetof(SBuf, L) > 32760) return 2;'
  printf '%s\n' '  printf("%tx %tx %zx %zx %tx\n", (o & 0xfff000) >> 12,'
  printf '%s\n' '         o & 0xfff, offsetof(SBuf, b), offsetof(SBuf, L), o);'
  printf '%s\n' '  return 0;'
  printf '%s\n' '}'
} >"$offset_probe"
cc=${CC:-cc}
if ! "$cc" -std=gnu11 -DLUAJIT_MT_ARM64_BOOTSTRAP \
    -DLUAJIT_DISABLE_JIT -I"$root/src" "$offset_probe" -o "$offset_exe"; then
  echo "ARM64 tmpbuf contract could not resolve the TG/SBuf offsets" >&2
  exit 1
fi
set -- $("$offset_exe")
if test "$#" -ne 5; then
  echo "ARM64 tmpbuf offset probe returned malformed output" >&2
  exit 1
fi
tmp_hi=$1
tmp_lo=$2
b_off=$3
l_off=$4
tmp_full=$5
if test "$tmp_full" = "$tmp_lo"; then
  echo "ARM64 tmpbuf offset unexpectedly fits a single unshifted ADD" >&2
  exit 1
fi

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

require_object_sequence() {
  object_symbol=$1
  object_range=$tmpdir/$object_symbol.disasm
  if ! extract_symbol_disasm "$object_symbol" "$object_range" ||
     ! awk -v hi="$tmp_hi" -v lo="$tmp_lo" -v bo="$b_off" -v ll="$l_off" '
       BEGIN {
         pattern[1] = "add[[:space:]]+x14, x25, #0x0*" hi ", lsl #12"
         pattern[2] = "add[[:space:]]+x0, x14, #0x0*" lo "([[:space:]]|$)"
         pattern[3] = "add[[:space:]]+x14, x0, #0x0*" bo "([[:space:]]|$)"
         pattern[4] = "ldar[[:space:]]+x8, \\[x14\\]"
         pattern[5] = "str[[:space:]]+x23, \\[x0, #0x0*" ll "\\]"
         pattern[6] = "stlr[[:space:]]+x8, \\[x0\\]"
       }
       state < 6 && $0 ~ pattern[state+1] { state++ }
       END { exit(state == 6 ? 0 : 1) }
     ' "$object_range"; then
    echo "ARM64 $object_symbol lacks the split TG tmpbuf acquire/release reset" >&2
    exit 1
  fi
  if grep -Eq 'add[[:space:]]+x0, x22,' "$object_range"; then
    echo "ARM64 $object_symbol still forms a global tmpbuf address" >&2
    exit 1
  fi
}

require_object_sequence lj_ff_string_reverse
require_object_sequence lj_ff_string_lower
require_object_sequence lj_ff_string_upper

if nm -u "$vm_object" "$archive" | grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "ARM64 tmpbuf artifacts import an atomic runtime helper" >&2
  exit 1
fi

echo "arm64_tmpbuf_contract OK: reverse/lower/upper use split TG tmpbuf LDAR/STLR reset"
