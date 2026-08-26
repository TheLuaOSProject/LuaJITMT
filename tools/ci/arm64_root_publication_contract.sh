#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_root_publication_contract SKIP: requires native macOS arm64"
  exit 0
fi

vm_source=$root/src/vm_arm64.dasc
vm_object=${LJ_ARM64_VM_OBJECT:-$root/src/lj_vm.o}
source_only=${LJ_ARM64_ROOTPUB_SOURCE_ONLY:-0}

require_source_region() {
  start=$1
  stop=$2
  pattern=$3
  description=$4
  if ! awk -v start="$start" -v stop="$stop" -v pattern="$pattern" '
    $0 ~ start && !inside { inside = 1; first = NR }
    inside && NR != first && $0 ~ stop {
      ended = 1
      exit found ? 0 : 1
    }
    inside && $0 ~ pattern { found = 1 }
    END { if (!inside || !ended || !found) exit 1 }
  ' "$vm_source"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

require_source_text() {
  pattern=$1
  description=$2
  if ! grep -Eq "$pattern" "$vm_source"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

publication='^[[:space:]]*[|][[:space:]]+(arm64_vm_stack_dirty|bl extern lj_state_stack_pubtv)'

require_source_region '[.]macro arm64_vm_stack_dirty' '[.]endmacro' \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_state_stack_dirty_vm' \
  'the assembly-callable stack-dirty helper macro'

# Frame transitions and result ranges. These paths may republish an existing
# root range by invalidating the scan rather than re-anchoring each element.
require_source_region '[.]macro ins_callt' '[.]macro ins_call' "$publication" \
  'Lua-call frame publication'
require_source_region '[|]->vm_return:' '[|]->vm_leave_cp:' "$publication" \
  'C-return result-range publication'
require_source_region '[|]->fff_res:' '[.]macro math_extern' "$publication" \
  'fast-function result publication'
require_source_region '[.]ffunc_1 next' '[.]ffunc_1 pairs' \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_tab_next_pair_rooted' \
  'rooted fast next iterator result helper'
require_source_region \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_tab_next_pair_rooted' \
  '[.]ffunc_1 pairs' "$publication" \
  'fast next iterator result publication'
require_source_region 'case BC_RETM:' 'case BC_FORL:' "$publication" \
  'Lua RET/RET0/RET1 result publication'

# Raw stack writes which can carry either collectable or scalar TValues.
require_source_region 'case BC_ISTC:' 'case BC_ISTYPE:' "$publication" \
  'taken test-copy publication'
require_source_region 'case BC_MOV:' 'case BC_NOT:' "$publication" \
  'MOV publication'
require_source_region '[|]->cont_cat:' '//-- Table indexing metamethods' \
  'bl extern lj_state_stack_pubtv' 'concatenation continuation root publication'
require_source_region '[|]->cont_ra:' '[|]->cont_condt:' \
  'bl extern lj_state_stack_pubtv' 'metamethod continuation root publication'
require_source_region 'case BC_CAT:' 'case BC_KSTR:' "$publication" \
  'direct concatenation-result publication'
require_source_region 'case BC_KSTR:' 'case BC_KCDATA:' "$publication" \
  'string-constant root publication'
require_source_region 'case BC_KCDATA:' 'case BC_KSHORT:' "$publication" \
  'cdata-constant root publication'
require_source_region 'case BC_UGET:' 'case BC_CNEW:' "$publication" \
  'closed-upvalue result publication'
require_source_region 'case BC_FNEW:' 'case BC_TNEW:' "$publication" \
  'new-function root publication'
require_source_region 'case BC_TNEW:' 'case BC_GGET:' "$publication" \
  'new/duplicate table root publication'

# Fast and metamethod table reads must both publish their completed output.
require_source_region '[|]->vmeta_tgetv:' '[|]->vmeta_tget_done:' \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_meta_tgettv_rooted' \
  'rooted metamethod table-read helper'
require_source_region '[|]->vmeta_tget_done:' '// Call __index metamethod' \
  "$publication" 'rooted metamethod table-read completion publication'
require_source_region '[|]->vmeta_tgetr:' '//-----' \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_tab_gettv_rooted' \
  'rooted raw table-read helper'
require_source_region \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_tab_gettv_rooted' \
  '//-----' "$publication" \
  'rooted raw table-read completion publication'
require_source_region 'case BC_TGETV:' 'case BC_TGETS:' \
  '^[[:space:]]*[|][[:space:]]+b ->vmeta_tgetv' \
  'unconditional TGETV rooted dispatch'
require_source_region 'case BC_TGETS:' 'case BC_TGETB:' \
  '^[[:space:]]*[|][[:space:]]+b ->vmeta_tgets' \
  'unconditional TGETS rooted dispatch'
require_source_region 'case BC_TGETB:' 'case BC_TGETR:' \
  '^[[:space:]]*[|][[:space:]]+b ->vmeta_tgetb' \
  'unconditional TGETB rooted dispatch'
require_source_region 'case BC_TGETR:' 'case BC_TSETV:' \
  '^[[:space:]]*[|][[:space:]]+b ->vmeta_tgetr' \
  'unconditional TGETR rooted dispatch'
require_source_region 'case BC_TSETV:' 'case BC_TSETS:' \
  '^[[:space:]]*[|][[:space:]]+b ->vmeta_tsetv' \
  'unconditional TSETV rooted dispatch'
require_source_region 'case BC_TSETS:' 'case BC_TSETB:' \
  '^[[:space:]]*[|][[:space:]]+b ->vmeta_tsets' \
  'unconditional TSETS rooted dispatch'
require_source_region 'case BC_TSETB:' 'case BC_TSETR:' \
  '^[[:space:]]*[|][[:space:]]+b ->vmeta_tsetb' \
  'unconditional TSETB rooted dispatch'

# Iterators, varargs and vararg-frame setup publish multi-slot ranges.
require_source_region 'case BC_ITERC:' 'case BC_ITERN:' "$publication" \
  'generic iterator frame publication'
require_source_region 'case BC_ITERN:' 'case BC_ISNEXT:' \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_tab_itern_rooted' \
  'rooted ITERN traversal'
require_source_region \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_tab_itern_rooted' \
  'case BC_ISNEXT:' "$publication" \
  'ITERN pair-result publication'
require_source_region 'case BC_VARG:' 'case BC_RETM:' "$publication" \
  'vararg destination-range publication'
require_source_region 'case BC_IFUNCV:' 'case BC_FUNCC:' "$publication" \
  'vararg function-frame publication'

# Error returns flow through the same certified return machinery.
require_source_region '[|]->vm_unwind_ff_eh:' '//-- Grow stack' \
  '^[[:space:]]*[|][[:space:]]+b ->vm_returnc' \
  'fast-function error return through vm_returnc'

for helper in \
  lj_meta_tgetenv_rooted \
  lj_meta_tgettv_rooted \
  lj_meta_tsetenvtv_pair \
  lj_meta_tsettv_pair \
  lj_tab_gettv_rooted \
  lj_tab_next_pair_rooted \
  lj_tab_itern_rooted
do
  require_source_text \
    "^[[:space:]]*[|][[:space:]]+bl extern $helper" "$helper call"
done

if test "$source_only" = 1; then
  echo "arm64_root_publication_contract OK: source publication families present"
  exit 0
fi

if test ! -f "$vm_object"; then
  echo "ARM64 root-publication contract needs a completed bootstrap build: $vm_object" >&2
  exit 1
fi
if test "$vm_source" -nt "$vm_object"; then
  echo "ARM64 root-publication contract needs a VM object newer than its source" >&2
  exit 1
fi
if ! file "$vm_object" | grep -Eq 'Mach-O 64-bit object arm64'; then
  echo "ARM64 root-publication contract received a non-arm64 VM object: $vm_object" >&2
  exit 1
fi
if test "$(lipo -archs "$vm_object" 2>/dev/null || true)" != arm64; then
  echo "ARM64 root-publication contract requires a thin arm64 VM object" >&2
  exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-rootpub.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
nm_text=$tmpdir/vm.nm
relocs=$tmpdir/vm.relocs
nm -n "$vm_object" >"$nm_text"
otool -rv "$vm_object" >"$relocs"

require_symbol_relocation() {
  symbol=$1
  target=$2
  description=$3
  if ! awk -v symbol="_$symbol" -v target="$target" '
    function normhex(s, z) {
      z = "0000000000000000" s
      return substr(z, length(z)-15)
    }
    FNR == NR {
      if ($2 == "T") {
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
    /^Relocation information \(__TEXT,__text\)/ {
      in_text = 1
      next
    }
    /^Relocation information / {
      in_text = 0
      next
    }
    in_text && $1 ~ /^[[:xdigit:]]+$/ {
      addr = normhex($1)
      if (found_symbol && ("x" addr) >= ("x" start) &&
          (end == "" || ("x" addr) < ("x" end)) && $NF ~ target)
        found_reloc = 1
    }
    END { exit found_symbol && found_reloc ? 0 : 1 }
  ' "$nm_text" "$relocs"; then
    echo "ARM64 $symbol lacks $description" >&2
    exit 1
  fi
}

pub_reloc='_(lj_state_stack_dirty_vm|lj_state_stack_pubtv)$'
for symbol in \
  lj_BC_CALL lj_vm_return lj_fff_res \
  lj_BC_ISTC lj_BC_MOV lj_BC_CAT lj_cont_ra \
  lj_BC_KSTR lj_BC_KCDATA lj_BC_UGET lj_BC_FNEW lj_BC_TNEW lj_BC_TDUP \
  lj_BC_ITERC lj_BC_ITERN lj_BC_VARG lj_BC_RET lj_BC_IFUNCV
do
  require_symbol_relocation "$symbol" "$pub_reloc" \
    'stack-dirty or rooted-stack publication relocation'
done

require_symbol_relocation lj_vmeta_tgetv '_lj_meta_tgettv_rooted$' \
  'rooted metamethod table-read relocation'
require_symbol_relocation lj_vmeta_tgetr '_lj_tab_gettv_rooted$' \
  'rooted raw table-read relocation'
require_symbol_relocation lj_ff_next '_lj_tab_next_pair_rooted$' \
  'rooted next-pair relocation'
require_symbol_relocation lj_ff_next "$pub_reloc" \
  'fast next iterator result publication relocation'
require_symbol_relocation lj_BC_ITERN '_lj_tab_itern_rooted$' \
  'rooted iterator relocation'

if nm -u "$vm_object" | grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "ARM64 VM root-publication paths import an atomic runtime helper" >&2
  exit 1
fi

echo "arm64_root_publication_contract OK: source and emitted publication families present"
