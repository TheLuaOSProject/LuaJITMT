#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_tg_dispatch_contract SKIP: requires native macOS arm64"
  exit 0
fi

vm_source=$root/src/vm_arm64.dasc
target_header=$root/src/lj_target_arm64.h
vm_object=${LJ_ARM64_VM_OBJECT:-$root/src/lj_vm.o}
dispatch_object=${LJ_ARM64_DISPATCH_OBJECT:-$root/src/lj_dispatch.o}

for artifact in "$vm_object" "$dispatch_object"; do
  if test ! -f "$artifact"; then
    echo "ARM64 TG dispatch contract needs a completed bootstrap build: $artifact" >&2
    exit 1
  fi
done

if ! grep -Eq '^\|[.]define DISPATCH,[[:space:]]+x25' "$vm_source" ||
   ! grep -Eq 'RID_DISPATCH[[:space:]]*=[[:space:]]*RID_X25' "$target_header" ||
   ! grep -Eq 'RID2RSET\(RID_DISPATCH\)' "$target_header"; then
  echo "ARM64 TG dispatch register is not fixed consistently at x25" >&2
  exit 1
fi
if grep -Eq 'TISNUMhi|GL->cur_L|GL->hookcount|GL->hookmask' "$vm_source"; then
  echo "ARM64 VM retains a raw global owner/hook access or the old x25 tag cache" >&2
  exit 1
fi
if ! grep -Eq 'sub TMP0, DISPATCH, #-TG_DISP2HOT' "$vm_source"; then
  echo "ARM64 hotcount addressing is not relative to the TG dispatch base" >&2
  exit 1
fi

# Reject global dispatch addressing in every source branch active when JIT is
# disabled. Unknown DynASM conditions are conservatively treated as active;
# only exact `.if JIT`/`.if not JIT` arms are folded here.
if ! awk '
  BEGIN { depth = 0; enabled[0] = 1 }
  /\|[[:space:]]*[.]if[[:space:]]+/ {
    line = $0
    sub(/^.*\|[[:space:]]*[.]if[[:space:]]+/, "", line)
    sub(/[[:space:]].*$/, "", line)
    depth++
    parent[depth] = enabled[depth-1]
    if (line == "JIT") {
      kind[depth] = 1
      enabled[depth] = 0
    } else if (line == "not") {
      rest = $0
      sub(/^.*\|[[:space:]]*[.]if[[:space:]]+not[[:space:]]+/, "", rest)
      sub(/[[:space:]].*$/, "", rest)
      if (rest == "JIT") {
        kind[depth] = 2
        enabled[depth] = parent[depth]
      } else {
        enabled[depth] = parent[depth]
      }
    } else {
      enabled[depth] = parent[depth]
    }
    next
  }
  /\|[[:space:]]*[.]else/ {
    if (kind[depth] == 1)
      enabled[depth] = parent[depth]
    else if (kind[depth] == 2)
      enabled[depth] = 0
    next
  }
  /\|[[:space:]]*[.]endif/ {
    delete enabled[depth]
    delete parent[depth]
    delete kind[depth]
    depth--
    next
  }
  enabled[depth] && /GG_G2DISP/ { bad = 1; print > "/dev/stderr" }
  END { exit bad ? 1 : 0 }
' "$vm_source"; then
  echo "ARM64 JIT-off source retains universe-global dispatch addressing" >&2
  exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-tg-dispatch.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
vm_disasm=$tmpdir/vm.disasm
dispatch_disasm=$tmpdir/dispatch.disasm
otool -tvV "$vm_object" >"$vm_disasm"
otool -tvV "$dispatch_object" >"$dispatch_disasm"

require_symbol_pattern() {
  disasm=$1
  symbol=$2
  pattern=$3
  description=$4
  if ! awk -v label="_$symbol:" -v pattern="$pattern" '
    $0 == label { inside = 1; next }
    inside && /^_/ { exit found ? 0 : 1 }
    inside && $0 ~ pattern { found = 1 }
    END { if (!inside || !found) exit 1 }
  ' "$disasm"; then
    echo "ARM64 $symbol lacks $description" >&2
    exit 1
  fi
}

require_symbol_count() {
  disasm=$1
  symbol=$2
  pattern=$3
  minimum=$4
  description=$5
  if ! awk -v label="_$symbol:" -v pattern="$pattern" -v minimum="$minimum" '
    $0 == label { inside = 1; next }
    inside && /^_/ { exit count >= minimum ? 0 : 1 }
    inside && $0 ~ pattern { count++ }
    END { if (!inside || count < minimum) exit 1 }
  ' "$disasm"; then
    echo "ARM64 $symbol lacks $description" >&2
    exit 1
  fi
}

require_symbol_pattern "$vm_disasm" lj_BC_MOV \
  'add[[:space:]]+x9, x25,' 'x25-based dynamic dispatch'
require_symbol_pattern "$vm_disasm" lj_BC_MOV \
  'ldr[[:space:]]+x8, \\[x9\\]' 'direct TG dispatch-table load'

for symbol in \
  lj_vm_resume \
  lj_vm_call \
  lj_vm_unwind_c_eh \
  lj_vm_unwind_ff_eh \
  lj_vm_ffi_callback
do
  require_symbol_pattern "$vm_disasm" "$symbol" \
    'ldar[[:space:]]+x25,' 'acquire TG dispatch reconstruction'
done
for symbol in lj_vm_resume lj_vm_call; do
  require_symbol_pattern "$vm_disasm" "$symbol" \
    'stlr[[:space:]]+x23,' 'release TG cur_L publication'
  require_symbol_count "$vm_disasm" "$symbol" \
    'stlr[[:space:]]+w26,' 2 'dual TG/global interpreter vmstate publication'
done
require_symbol_count "$vm_disasm" lj_vm_unwind_c_eh \
  'stlr[[:space:]]+w8,' 2 'dual TG/global unwind vmstate publication'
require_symbol_count "$vm_disasm" lj_vm_unwind_ff_eh \
  'stlr[[:space:]]+w26,' 2 'dual TG/global fast-unwind vmstate publication'
require_symbol_count "$vm_disasm" lj_vm_ffi_callback \
  'stlr[[:space:]]+w26,' 2 'dual TG/global callback vmstate publication'
require_symbol_pattern "$vm_disasm" lj_BC_FUNCC \
  'stlr[[:space:]]+x23,' 'release TG cur_L publication after a C call'
require_symbol_pattern "$vm_disasm" lj_vm_inshook \
  'ldarb[[:space:]]+w10,' 'acquire TG hook-mask load'
require_symbol_pattern "$vm_disasm" lj_vm_inshook \
  'ldarb[[:space:]]+w11,' 'acquire global hook-mask load'
require_symbol_pattern "$vm_disasm" lj_vm_inshook \
  'add[[:space:]]+x9, x25,' 'x25-based static redispatch reconstruction'
require_symbol_pattern "$vm_disasm" lj_vm_inshook \
  'ldr[[:space:]]+x8, \\[x9, #' 'TG static-dispatch load'

if ! nm -u "$vm_object" | grep '_lj_dispatch_hookcount_dec' >/dev/null; then
  echo "ARM64 VM does not call the atomic hook-count decrement helper" >&2
  exit 1
fi
if ! awk '
  $0 == "_lj_dispatch_hookcount_dec:" { inside = 1; next }
  inside && /^_/ { exit lse || (loadx && storex) ? 0 : 1 }
  inside && /ldaddal/ { lse = 1 }
  inside && /ldaxr/ { loadx = 1 }
  inside && /stlxr/ { storex = 1 }
  END { if (!inside || !(lse || (loadx && storex))) exit 1 }
' "$dispatch_disasm"; then
  echo "ARM64 hook-count helper lacks an inline acquire-release RMW" >&2
  exit 1
fi

if nm -u "$vm_object" "$dispatch_object" | \
   grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "ARM64 TG dispatch objects import an atomic runtime helper" >&2
  exit 1
fi

echo "arm64_tg_dispatch_contract OK: x25 TG dispatch and atomic publication are present"
