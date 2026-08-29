#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_meta_publication_contract SKIP: requires native macOS arm64"
  exit 0
fi

vm_source=$root/src/vm_arm64.dasc
meta_source=$root/src/lj_meta.c
meta_header=$root/src/lj_meta.h
base_source=$root/src/lib_base.c
vm_object=${LJ_ARM64_VM_OBJECT:-$root/src/lj_vm.o}
meta_object=${LJ_ARM64_META_OBJECT:-$root/src/lj_meta.o}
base_object=${LJ_ARM64_BASE_OBJECT:-$root/src/lib_base.o}
archive=${LJ_ARM64_ARCHIVE:-$root/src/libluajit.a}
runtime_exe=${LJ_ARM64_RUNTIME:-$root/src/luajit}
source_only=${LJ_ARM64_METAPUB_SOURCE_ONLY:-0}

require_source_region() {
  file=$1
  start=$2
  stop=$3
  pattern=$4
  description=$5
  if ! awk -v start="$start" -v stop="$stop" -v pattern="$pattern" '
    $0 ~ start && !inside { inside = 1; first = NR }
    inside && NR != first && $0 ~ stop {
      ended = 1
      exit found ? 0 : 1
    }
    inside && $0 ~ pattern { found = 1 }
    END { if (!inside || !ended || !found) exit 1 }
  ' "$file"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

require_source_region_count() {
  file=$1
  start=$2
  stop=$3
  pattern=$4
  minimum=$5
  description=$6
  if ! awk -v start="$start" -v stop="$stop" -v pattern="$pattern" \
      -v minimum="$minimum" '
    $0 ~ start && !inside { inside = 1; first = NR }
    inside && NR != first && $0 ~ stop {
      ended = 1
      exit count >= minimum ? 0 : 1
    }
    inside && $0 ~ pattern { count++ }
    END { if (!inside || !ended || count < minimum) exit 1 }
  ' "$file"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

require_source_next_instruction() {
  file=$1
  start=$2
  stop=$3
  pattern=$4
  description=$5
  if ! awk -v start="$start" -v stop="$stop" -v pattern="$pattern" '
    $0 ~ start && !inside { inside = 1; next }
    inside && $0 ~ stop { exit }
    inside && $0 ~ /^[[:space:]]*[|][[:space:]]*(\/\/|$)/ { next }
    inside && $0 ~ /^[[:space:]]*[|]/ {
      checked = 1
      matched = ($0 ~ pattern)
      exit
    }
    END { exit inside && checked && matched ? 0 : 1 }
  ' "$file"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

require_source_order() {
  file=$1
  start=$2
  stop=$3
  first=$4
  second=$5
  description=$6
  if ! awk -v start="$start" -v stop="$stop" -v first="$first" \
      -v second="$second" '
    $0 ~ start && !inside { inside = 1; firstline = NR }
    inside && NR != firstline && $0 ~ stop {
      ended = 1
      exit ordered ? 0 : 1
    }
    inside && $0 ~ first { seen_first = 1 }
    inside && seen_first && $0 ~ second { ordered = 1 }
    END { if (!inside || !ended || !ordered) exit 1 }
  ' "$file"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

require_source_sequence3() {
  file=$1
  start=$2
  stop=$3
  first=$4
  second=$5
  third=$6
  description=$7
  if ! awk -v start="$start" -v stop="$stop" -v first="$first" \
      -v second="$second" -v third="$third" '
    $0 ~ start && !inside { inside = 1; firstline = NR }
    inside && NR != firstline && $0 ~ stop {
      ended = 1
      exit state == 3 ? 0 : 1
    }
    inside && state == 0 && $0 ~ first { state = 1; next }
    inside && state == 1 && $0 ~ second { state = 2; next }
    inside && state == 2 && $0 ~ third { state = 3 }
    END { if (!inside || !ended || state != 3) exit 1 }
  ' "$file"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

require_source_sequence4() {
  file=$1
  start=$2
  stop=$3
  first=$4
  second=$5
  third=$6
  fourth=$7
  description=$8
  if ! awk -v start="$start" -v stop="$stop" -v first="$first" \
      -v second="$second" -v third="$third" -v fourth="$fourth" '
    $0 ~ start && !inside { inside = 1; firstline = NR }
    inside && NR != firstline && $0 ~ stop {
      ended = 1
      exit state == 4 ? 0 : 1
    }
    inside && state == 0 && $0 ~ first { state = 1; next }
    inside && state == 1 && $0 ~ second { state = 2; next }
    inside && state == 2 && $0 ~ third { state = 3; next }
    inside && state == 3 && $0 ~ fourth { state = 4 }
    END { if (!inside || !ended || state != 4) exit 1 }
  ' "$file"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

require_source_adjacent_instructions() {
  file=$1
  start=$2
  stop=$3
  first=$4
  second=$5
  description=$6
  if ! awk -v start="$start" -v stop="$stop" -v first="$first" \
      -v second="$second" '
    $0 ~ start && !inside { inside = 1; firstline = NR; next }
    inside && NR != firstline && $0 ~ stop {
      ended = 1
      exit matched ? 0 : 1
    }
    inside && $0 ~ /^[[:space:]]*[|][[:space:]]*(\/\/|$)/ { next }
    inside && $0 ~ /^[[:space:]]*[|][[:space:]]*[0-9]+:/ { next }
    inside && $0 ~ /^[[:space:]]*[|][[:space:]]+/ {
      if (waiting) {
        matched = ($0 ~ second)
        waiting = 0
      } else if ($0 ~ first) {
        waiting = 1
      }
    }
    END { if (!inside || !ended || !matched) exit 1 }
  ' "$file"; then
    echo "ARM64 source lacks $description" >&2
    exit 1
  fi
}

reject_source_region() {
  file=$1
  start=$2
  stop=$3
  pattern=$4
  description=$5
  if ! awk -v start="$start" -v stop="$stop" -v pattern="$pattern" '
    $0 ~ start && !inside { inside = 1; first = NR }
    inside && NR != first && $0 ~ stop {
      ended = 1
      exit found ? 1 : 0
    }
    inside && $0 ~ pattern { found = 1 }
    END { if (!inside || !ended || found) exit 1 }
  ' "$file"; then
    echo "ARM64 source contains forbidden $description" >&2
    exit 1
  fi
}

dyn_call='^[[:space:]]*[|][[:space:]]+bl extern '
publication='^[[:space:]]*[|][[:space:]]+(arm64_vm_stack_dirty|bl extern lj_state_stack_pubtv)'

# Mutable 64-bit VM roots, edges, closure slots and callback words are all
# release-published by their C-side accessors. Pin the target-local acquire
# primitive and every VM family which consumes those publications. The Lua 5.2
# iterator metatable checks need source proof because the default object omits
# those conditional instructions.
require_source_region_count "$vm_source" \
  '[.]macro arm64_vm_u64_acq, dst, base, ofs' \
  '[.]macro arm64_vm_tab_asize_acq' \
  '^[[:space:]]*[|][[:space:]]+ldar dst, [[]ATMP[]]' 2 \
  'constant/indexed 64-bit acquire-load primitive'
require_source_region "$vm_source" '[.]ffunc_1 type' \
  '//-- Base library: getters and setters' \
  'arm64_vm_u64_acq CARG1, CFUNC:CARG3, TMP1, 3' \
  'type() C-closure upvalue acquire'
require_source_region "$vm_source" '[.]ffunc_1 tostring' \
  '//-- Base library: iterators' \
  'arm64_vm_u64_acq TMP1, GLREG, GL_OFS[(]gcroot[)][+]8[*]GCROOT_BASEMT_NUM' \
  'tostring() numeric base-metatable root acquire'
require_source_sequence3 "$vm_source" '[.]ffunc_1 pairs' \
  '[.]ffunc_2 ipairs_aux' '^#if LJ_52$' \
  'arm64_vm_u64_acq TAB:CARG2, TAB:TMP1, offsetof[(]GCtab, metatable[)]' \
  '^#endif$' 'Lua 5.2 pairs() metatable acquire guard'
require_source_region "$vm_source" '[.]ffunc_1 pairs' \
  '[.]ffunc_2 ipairs_aux' \
  'arm64_vm_u64_acq CFUNC:CARG4, CFUNC:CARG3, offsetof[(]GCfuncC, upvalue[)]' \
  'pairs() C-closure upvalue acquire'
require_source_sequence3 "$vm_source" '[.]ffunc_1 ipairs' \
  '//-- Base library: catch errors' '^#if LJ_52$' \
  'arm64_vm_u64_acq TAB:CARG2, TAB:TMP1, offsetof[(]GCtab, metatable[)]' \
  '^#endif$' 'Lua 5.2 ipairs() metatable acquire guard'
require_source_region "$vm_source" '[.]ffunc_1 ipairs' \
  '//-- Base library: catch errors' \
  'arm64_vm_u64_acq CFUNC:CARG4, CFUNC:CARG3, offsetof[(]GCfuncC, upvalue[)]' \
  'ipairs() C-closure upvalue acquire'
require_source_region "$vm_source" '[.]ffunc coroutine_wrap_aux' \
  '[.]endif' \
  'arm64_vm_u64_acq L:CARG1, CFUNC:CARG3, offsetof[(]GCfuncC, upvalue[)]' \
  'coroutine.wrap() C-closure upvalue acquire'
require_source_region_count "$vm_source" 'case BC_UGET:' 'case BC_UCLO:' \
  'arm64_vm_u64_acq UPVAL:CARG[12], LFUNC:CARG2, (RC|RA), 3' 5 \
  'all Lua-closure uvptr acquires'
require_source_region "$vm_source" 'case BC_UCLO:' 'case BC_FNEW:' \
  'arm64_vm_u64_acq CARG3, LREG, L_OFS[(]openupval[)]' \
  'UCLO open-upvalue head acquire'
require_source_region "$vm_source" 'case BC_FUNCC:' '^[[:space:]]+default:' \
  'arm64_vm_u64_acq CARG4, GLREG, GL_OFS[(]wrapf[)]' \
  'FUNCCW wrapper callback acquire'

# Metamethod lookup/call boundaries used by comparisons, arithmetic, length,
# callable values and concatenation.
require_source_region "$vm_source" '[|]->vmeta_comp:' '[|]->cont_ra:' \
  "${dyn_call}lj_meta_comp_rooted" '__lt/__le rooted comparison helper dispatch'
require_source_region "$vm_source" '[|]->vmeta_equal:' \
  '[|]->vmeta_equal_cd:' "${dyn_call}lj_meta_equal_rooted" \
  'rooted table __eq helper dispatch'
require_source_region "$vm_source" '[|]->vmeta_equal_cd:' \
  '[|]->vmeta_istype:' "${dyn_call}lj_meta_equal_cd" \
  'FFI metatype __eq helper dispatch'
require_source_region "$vm_source" '[|]->vmeta_arith_vv:' \
  '[|]->vmeta_binop:' "${dyn_call}lj_meta_arith" \
  '__add arithmetic helper dispatch'
require_source_region "$vm_source" '[|]->vmeta_len:' \
  '//-- Call metamethod' "${dyn_call}lj_meta_len" \
  '__len helper dispatch'
require_source_region "$vm_source" '[|]->vmeta_call:' \
  '[|]->vmeta_callt:' "${dyn_call}lj_meta_call" \
  '__call helper dispatch'
require_source_region "$vm_source" '[|]->vmeta_callt:' \
  '//-- Argument coercion' "${dyn_call}lj_meta_call" \
  'tail-call __call helper dispatch'
require_source_region "$vm_source" 'case BC_CAT:' 'case BC_KSTR:' \
  "${dyn_call}lj_meta_cat" '__concat helper dispatch'

# Ordered comparison and distinct-object equality pass authoritative stack
# addresses to helpers which may retry and relocate the stack. Both return via
# the common label which reloads BASE before inspecting the helper result.
require_source_region_count "$vm_source" \
  'case BC_ISLT: case BC_ISGE: case BC_ISLE: case BC_ISGT:' \
  'case BC_ISEQV: case BC_ISNEV:' \
  '^[[:space:]]*[|][[:space:]]+b(lo|ne)[[:space:]]+->vmeta_comp' 3 \
  'all ordered-comparison nonnumeric routes to vmeta_comp'
require_source_region "$vm_source" '[|]->vmeta_comp:' '[|]->cont_ra:' \
  'add CARG2, BASE, RA, lsl #3' '__lt/__le lhs stack-root address'
require_source_region "$vm_source" '[|]->vmeta_comp:' '[|]->cont_ra:' \
  'add CARG3, BASE, RC, lsl #3' '__lt/__le rhs stack-root address'
require_source_sequence3 "$vm_source" '[|]->vmeta_comp:' '[|]->cont_ra:' \
  "${dyn_call}lj_meta_comp_rooted" \
  '^[[:space:]]*[|]3:' \
  '^[[:space:]]*[|][[:space:]]+ldr BASE, L->base' \
  '__lt/__le helper return through shared label 3 BASE reload'
require_source_adjacent_instructions "$vm_source" \
  '[|]->vmeta_comp:' '[|]->cont_ra:' \
  '^[[:space:]]*[|][[:space:]]+ldr BASE, L->base' \
  '^[[:space:]]*[|][[:space:]]+cmp CRET1, #1' \
  'shared label 3 immediate BASE reload/result compare'
require_source_region "$vm_source" 'case BC_ISEQV: case BC_ISNEV:' \
  'case BC_ISEQS:' 'add CARG2, BASE, RA, lsl #3' \
  'table equality lhs stack-root address'
require_source_sequence3 "$vm_source" \
  'case BC_ISEQV: case BC_ISNEV:' 'case BC_ISEQS:' \
  'add RC, BASE, RC, lsl #3' 'mov CARG3, RC' \
  '^[[:space:]]*[|][[:space:]]+b ->vmeta_equal' \
  'table equality rhs stack rebase and rooted-helper route'
reject_source_region "$vm_source" 'case BC_ISEQV: case BC_ISNEV:' \
  'case BC_ISEQS:' '(->metatable|->nomm|MM_eq)' \
  'raw table equality metatable/nomm prefilter'
require_source_adjacent_instructions "$vm_source" '[|]->vmeta_equal:' \
  '[|]->vmeta_equal_cd:' "${dyn_call}lj_meta_equal_rooted" \
  '^[[:space:]]*[|][[:space:]]+b <3' \
  'table equality helper return directly to shared BASE reload'

# The C helpers must retain both stack operands across lookup retries. Equality
# additionally preserves Lua 5.1's same-handler rule even for distinct
# metatables, while comparison retains its reversed-__lt fallback operands.
require_source_region "$meta_header" 'lj_meta_equal_rooted' \
  'lj_meta_equal[(]' 'LJ_FUNCA TValue [*]lj_meta_equal_rooted' \
  'assembler-visible rooted equality declaration'
require_source_region "$meta_source" '^TValue [*]lj_meta_equal_rooted[(]' \
  '^TValue [*]lj_meta_equal[(]' 'meta_chain_capture_inputs' \
  'rooted equality operand capture'
require_source_region "$meta_source" '^TValue [*]lj_meta_equal_rooted[(]' \
  '^TValue [*]lj_meta_equal[(]' 'lj_obj_equal[(]method, method2[)]' \
  'rooted equality same-handler enforcement'
require_source_order "$meta_source" '^TValue [*]lj_meta_equal_rooted[(]' \
  '^TValue [*]lj_meta_equal[(]' 'meta_chain_mmcall[(]L,' \
  'meta_chain_roots_fini[(]&roots[)]' \
  'rooted equality call-frame setup before root cleanup'
require_source_region "$meta_header" 'lj_meta_comp_rooted' \
  'lj_meta_comp[(]' 'LJ_FUNCA TValue [*]lj_meta_comp_rooted' \
  'assembler-visible rooted comparison declaration'
require_source_region "$meta_source" '^TValue [*]lj_meta_comp_rooted[(]' \
  '^TValue [*]lj_meta_comp[(]' 'meta_chain_capture_inputs' \
  'comparison operand rooting'
require_source_region "$meta_source" '^TValue [*]lj_meta_comp_rooted[(]' \
  '^TValue [*]lj_meta_comp[(]' 'swapped = !swapped' \
  'comparison reversed-__lt rooted fallback'
require_source_region "$meta_source" '^TValue [*]lj_meta_comp_rooted[(]' \
  '^TValue [*]lj_meta_comp[(]' 'lj_obj_equal[(]method, method2[)]' \
  'Lua 5.1 comparison same-handler enforcement'
require_source_region_count "$meta_source" \
  '^TValue [*]lj_meta_comp_rooted[(]' '^TValue [*]lj_meta_comp[(]' \
  'meta_chain_mmcall[(]L,' 2 \
  'rooted comparison success call-frame paths'
require_source_order "$meta_source" '^TValue [*]lj_meta_comp_rooted[(]' \
  '^TValue [*]lj_meta_comp[(]' 'meta_chain_mmcall[(]L,' \
  'meta_chain_roots_fini[(]&roots[)]' \
  'rooted comparison call-frame setup before root cleanup'
require_source_sequence4 "$meta_source" '^TValue [*]lj_meta_comp_rooted[(]' \
  '^TValue [*]lj_meta_comp[(]' 'tv_rawstore[(]&err1,' \
  'tv_rawstore[(]&err2,' 'meta_chain_roots_fini[(]&roots[)]' \
  'lj_err_comp[(]L, &err1, &err2[)]' \
  'both comparison error snapshots and anchor cleanup before throw'

# Each C-side metamethod setup writes three collectable stack slots: method,
# left/receiver and right/argument. Publication must cover all three writes;
# a generic VM call/return dirty bump cannot make these pre-dispatch roots safe.
require_source_region_count "$meta_source" \
  '^static TValue [*]mmcall[(]' '^/[*] -- C helpers' \
  '^[[:space:]]+lj_state_stack_pubtv[(]L, L,' 3 \
  'mmcall method/argument three-slot publication'
require_source_region_count "$meta_source" \
  '^TValue [*]lj_meta_cat[(]' '^/[*] Helper for LEN[.]' \
  '^[[:space:]]+lj_state_stack_pubtv[(]L, L,' 3 \
  '__concat method/operand three-slot publication'
require_source_region_count "$meta_source" \
  '^void lj_meta_call[(]' '^/[*] Helper for FORI[.]' \
  '^[[:space:]]+lj_state_stack_pubtv[(]L, L,' 3 \
  '__call shifted-frame three-site publication'

# For C-built frames, prove that each representative TValue write precedes its
# release publication. Counts alone would also accept a publication performed
# before the collectable value became visible in the destination slot.
require_source_order "$meta_source" '^static TValue [*]mmcall[(]' \
  '^/[*] -- C helpers' 'copyTV[(]L, top[+][+], mo[)]' \
  'lj_state_stack_pubtv[(]L, L, metafunc[)]' \
  'mmcall metamethod write-before-publication ordering'
require_source_order "$meta_source" '^static TValue [*]mmcall[(]' \
  '^/[*] -- C helpers' 'copyTV[(]L, top, a[)]' \
  'lj_state_stack_pubtv[(]L, L, top[)]' \
  'mmcall first-argument write-before-publication ordering'
require_source_order "$meta_source" '^static TValue [*]mmcall[(]' \
  '^/[*] -- C helpers' 'copyTV[(]L, top[+]1, b[)]' \
  'lj_state_stack_pubtv[(]L, L, top[+]1[)]' \
  'mmcall second-argument write-before-publication ordering'
require_source_order "$meta_source" '^TValue [*]lj_meta_cat[(]' \
  '^/[*] Helper for LEN[.]' 'copyTV[(]L, metafunc, mo[)]' \
  'lj_state_stack_pubtv[(]L, L, metafunc[)]' \
  '__concat method write-before-publication ordering'
require_source_order "$meta_source" '^TValue [*]lj_meta_cat[(]' \
  '^/[*] Helper for LEN[.]' 'copyTV[(]L, metaarg1, top-1[)]' \
  'lj_state_stack_pubtv[(]L, L, metaarg1[)]' \
  '__concat first-operand write-before-publication ordering'
require_source_order "$meta_source" '^TValue [*]lj_meta_cat[(]' \
  '^/[*] Helper for LEN[.]' 'copyTV[(]L, metaarg2, top[)]' \
  'lj_state_stack_pubtv[(]L, L, metaarg2[)]' \
  '__concat second-operand write-before-publication ordering'
require_source_order "$meta_source" '^void lj_meta_call[(]' \
  '^/[*] Helper for FORI[.]' 'copyTV[(]L, p, p-1[)]' \
  'lj_state_stack_pubtv[(]L, L, p[)]' \
  '__call shifted-slot write-before-publication ordering'
require_source_order "$meta_source" '^void lj_meta_call[(]' \
  '^/[*] Helper for FORI[.]' 'copyTV[(]L, func, mo[)]' \
  'lj_state_stack_pubtv[(]L, L, func[)]' \
  '__call method write-before-publication ordering'

# Result paths must release-publish copied/returned TValues or invalidate the
# complete result range after its release stores.
require_source_region "$vm_source" '[|]->cont_ra:' '[|]->cont_condt:' \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_state_stack_pubtv' \
  'metamethod continuation result publication'
require_source_region "$vm_source" '[|]->cont_cat:' \
  '//-- Table indexing metamethods' \
  '^[[:space:]]*[|][[:space:]]+bl extern lj_state_stack_pubtv' \
  '__concat continuation result publication'
require_source_region "$vm_source" 'case BC_CAT:' 'case BC_KSTR:' \
  "$publication" 'direct concatenation result publication'
require_source_region "$vm_source" '[|]->fff_res:' '[.]macro math_extern' \
  "$publication" 'fast-function result publication'
require_source_region "$vm_source" '[|]->vm_return:' '[|]->vm_leave_cp:' \
  "$publication" '__call/C result-range publication'
require_source_region "$vm_source" 'case BC_RETM:' 'case BC_FORL:' \
  "$publication" '__call/Lua return-range publication'

# getmetatable() must use the rooted C helper which acquires the metatable edge,
# applies __metatable protection and writes to an authoritative result root.
# setmetatable() has no concurrency-safe inline subset: every case must use the
# real C implementation for type checks, replacement/clear and protection.
require_source_region "$vm_source" '[.]ffunc_1 getmetatable' \
  '[.]ffunc_2 setmetatable' \
  "${dyn_call}lj_meta_getmt_protected_rooted" \
  'rooted protected getmetatable helper dispatch'
require_source_region "$vm_source" '[.]ffunc_1 getmetatable' \
  '[.]ffunc_2 setmetatable' \
  '^[[:space:]]*[|][[:space:]]+b ->fff_res1([[:space:]]|$)' \
  'exact getmetatable published-result route'
require_source_order "$vm_source" '[.]ffunc_1 getmetatable' \
  '[.]ffunc_2 setmetatable' 'ldr PC, [[]BASE, FRAME_PC[]]' \
  'str PC, SAVE_PC' 'getmetatable frame-PC save ordering'
require_source_order "$vm_source" '[.]ffunc_1 getmetatable' \
  '[.]ffunc_2 setmetatable' 'mov CARG1, L' 'mov CARG2, BASE' \
  'getmetatable L/object-root argument ordering'
require_source_order "$vm_source" '[.]ffunc_1 getmetatable' \
  '[.]ffunc_2 setmetatable' 'mov CARG2, BASE' \
  'sub CARG3, BASE, #16' 'getmetatable object/output-root ABI'
require_source_order "$vm_source" '[.]ffunc_1 getmetatable' \
  '[.]ffunc_2 setmetatable' "${dyn_call}lj_meta_getmt_protected_rooted" \
  'ldr BASE, L->base' 'getmetatable post-helper BASE reload'
require_source_order "$vm_source" '[.]ffunc_1 getmetatable' \
  '[.]ffunc_2 setmetatable' 'ldr BASE, L->base' \
  'ldr PC, [[]BASE, FRAME_PC[]]' 'getmetatable post-helper PC reload'
require_source_order "$vm_source" '[.]ffunc_1 getmetatable' \
  '[.]ffunc_2 setmetatable' 'ldr PC, [[]BASE, FRAME_PC[]]' \
  '^[[:space:]]*[|][[:space:]]+b ->fff_res1([[:space:]]|$)' \
  'getmetatable PC reload before result dispatch'
require_source_next_instruction "$vm_source" '[.]ffunc_2 setmetatable' \
  '[.]ffunc rawget' \
  '^[[:space:]]*[|][[:space:]]+b ->fff_fallback([[:space:]]|$)' \
  'unconditional setmetatable C fallback route'

require_source_region "$meta_header" 'lj_meta_getmt_protected_rooted' \
  'lj_meta_tset[(]' 'LJ_FUNCA TValue [*]lj_meta_getmt_protected_rooted' \
  'assembler-visible protected getmetatable declaration'
require_source_region "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'meta_source_ref[(]L, objroot[)]' \
  'protected getmetatable relocatable object root'
require_source_region "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'meta_output_ref[(]L, outroot[)]' \
  'protected getmetatable relocatable output root'
require_source_region "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'lj_gc2_tv_lease_acquire[(]g, &osnap, &objlease[)]' \
  'protected getmetatable receiver admission'
require_source_region "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'lj_gc2_tv_lease_acquire[(]g, &mtv, &mtlease[)]' \
  'protected getmetatable metatable-generation admission'
require_source_region "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'lj_tab_getstr_held_try' 'protected getmetatable held lookup'
require_source_region "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'lj_gc2_tv_lease_acquire[(]g, &result, &resultlease[)]' \
  'protected getmetatable result admission'
require_source_region "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'lj_state_stack_pubtv[(]L, L, out[)]' \
  'protected getmetatable stack-result publication'
require_source_order "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'lj_gc2_tv_lease_acquire[(]g, &mtv, &mtlease[)]' \
  'lj_tab_getstr_held_try' \
  'protected getmetatable generation admission before held lookup'
require_source_order "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'lj_tab_getstr_held_try' \
  'lj_gc2_tv_lease_acquire[(]g, &result, &resultlease[)]' \
  'protected getmetatable held lookup before result admission'
require_source_order "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'copyTVrel[(]L, out, &result[)]' \
  'lj_state_stack_pubtv[(]L, L, out[)]' \
  'protected-token write-before-publication ordering'
require_source_order "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'copyTVrel[(]L, out, &mtv[)]' \
  'lj_state_stack_pubtv[(]L, L, out[)]' \
  'raw-metatable write-before-publication ordering'
require_source_order "$meta_source" \
  '^TValue [*]lj_meta_getmt_protected_rooted[(]' \
  '^static LJ_AINLINE TValue [*]meta_chain_root' \
  'lj_state_stack_pubtv[(]L, L, out[)]' \
  'lj_gc2_lease_release[(]&resultlease[)]' \
  'protected getmetatable publication before result release'

# The C fallback owns replacement, clear, protected and type-error semantics.
require_source_region "$base_source" 'LJLIB_ASM[(]setmetatable[)]' \
  'LJLIB_CF[(]getfenv[)]' 'lj_meta_lookuptv' \
  'protected-metatable lookup'
require_source_region "$base_source" 'LJLIB_ASM[(]setmetatable[)]' \
  'LJLIB_CF[(]getfenv[)]' 'LJ_ERR_PROTMT' \
  'protected-metatable error'
require_source_region "$base_source" 'LJLIB_ASM[(]setmetatable[)]' \
  'LJLIB_CF[(]getfenv[)]' 'lj_tab_metatable_rel' \
  'fallback metatable release publication'
require_source_region "$base_source" 'LJLIB_ASM[(]setmetatable[)]' \
  'LJLIB_CF[(]getfenv[)]' 'lj_gc_pubtabobj' \
  'fallback metatable GC edge publication'

if test "$source_only" = 1; then
  echo "arm64_meta_publication_contract OK: source families present"
  exit 0
fi

if test ! -f "$vm_object"; then
  echo "ARM64 metamethod publication contract needs a completed bootstrap build: $vm_object" >&2
  exit 1
fi
if test "$vm_source" -nt "$vm_object"; then
  echo "ARM64 metamethod publication contract needs a VM object newer than its source" >&2
  exit 1
fi
if test ! -f "$meta_object"; then
  echo "ARM64 metamethod publication contract needs a completed meta build: $meta_object" >&2
  exit 1
fi
if test "$meta_source" -nt "$meta_object"; then
  echo "ARM64 metamethod publication contract needs a meta object newer than its source" >&2
  exit 1
fi
if test "$meta_header" -nt "$meta_object"; then
  echo "ARM64 metamethod publication contract needs a meta object newer than its declarations" >&2
  exit 1
fi
if test ! -f "$base_object"; then
  echo "ARM64 metamethod publication contract needs a completed base-library build: $base_object" >&2
  exit 1
fi
if test "$base_source" -nt "$base_object"; then
  echo "ARM64 metamethod publication contract needs a base-library object newer than its source" >&2
  exit 1
fi
if test ! -f "$archive"; then
  echo "ARM64 metamethod publication contract needs a completed runtime archive: $archive" >&2
  exit 1
fi
if test "$vm_object" -nt "$archive" || test "$meta_object" -nt "$archive" || \
   test "$base_object" -nt "$archive"; then
  echo "ARM64 metamethod publication contract needs an archive newer than its VM/meta/base objects" >&2
  exit 1
fi
if test ! -x "$runtime_exe"; then
  echo "ARM64 metamethod publication contract needs a completed runtime executable: $runtime_exe" >&2
  exit 1
fi
if test "$archive" -nt "$runtime_exe"; then
  echo "ARM64 metamethod publication contract needs a runtime executable newer than its archive" >&2
  exit 1
fi
if ! file "$vm_object" | grep -Eq 'Mach-O 64-bit object arm64'; then
  echo "ARM64 metamethod publication contract received a non-arm64 VM object" >&2
  exit 1
fi
if ! file "$meta_object" | grep -Eq 'Mach-O 64-bit object arm64'; then
  echo "ARM64 metamethod publication contract received a non-arm64 meta object" >&2
  exit 1
fi
if ! file "$base_object" | grep -Eq 'Mach-O 64-bit object arm64'; then
  echo "ARM64 metamethod publication contract received a non-arm64 base object" >&2
  exit 1
fi
if test "$(lipo -archs "$vm_object" 2>/dev/null || true)" != arm64; then
  echo "ARM64 metamethod publication contract requires a thin arm64 VM object" >&2
  exit 1
fi
if test "$(lipo -archs "$meta_object" 2>/dev/null || true)" != arm64; then
  echo "ARM64 metamethod publication contract requires a thin arm64 meta object" >&2
  exit 1
fi
if test "$(lipo -archs "$base_object" 2>/dev/null || true)" != arm64; then
  echo "ARM64 metamethod publication contract requires a thin arm64 base object" >&2
  exit 1
fi
if test "$(lipo -archs "$archive" 2>/dev/null || true)" != arm64; then
  echo "ARM64 metamethod publication contract requires a thin arm64 runtime archive" >&2
  exit 1
fi
if test "$(lipo -archs "$runtime_exe" 2>/dev/null || true)" != arm64; then
  echo "ARM64 metamethod publication contract requires a thin arm64 runtime executable" >&2
  exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-metapub.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
nm_text=$tmpdir/vm.nm
relocs=$tmpdir/vm.relocs
disasm=$tmpdir/vm.disasm
meta_nm_text=$tmpdir/meta.nm
meta_relocs=$tmpdir/meta.relocs
nm -n "$vm_object" >"$nm_text"
otool -rv "$vm_object" >"$relocs"
otool -tvV "$vm_object" >"$disasm"
nm -n "$meta_object" >"$meta_nm_text"
otool -rv "$meta_object" >"$meta_relocs"

require_object_symbol_relocation() {
  object_nm=$1
  object_relocs=$2
  symbol=$3
  target=$4
  description=$5
  if ! awk -v symbol="_$symbol" -v target="$target" '
    function normhex(s, z) {
      z = "0000000000000000" s
      return substr(z, length(z)-15)
    }
    FNR == NR {
      if ($2 == "T" || $2 == "t") {
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
  ' "$object_nm" "$object_relocs"; then
    echo "ARM64 $symbol lacks $description" >&2
    exit 1
  fi
}

require_symbol_relocation() {
  require_object_symbol_relocation "$nm_text" "$relocs" "$1" "$2" "$3"
}

require_defined_text_symbol() {
  object_nm=$1
  symbol=$2
  description=$3
  if ! awk -v symbol="_$symbol" '
    $2 == "T" && $3 == symbol { found = 1 }
    END { exit found ? 0 : 1 }
  ' "$object_nm"; then
    echo "ARM64 object lacks $description: $symbol" >&2
    exit 1
  fi
}

require_symbol_instruction() {
  symbol=$1
  pattern=$2
  description=$3
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

require_symbol_instruction_count() {
  symbol=$1
  pattern=$2
  minimum=$3
  description=$4
  if ! awk -v label="_$symbol:" -v pattern="$pattern" \
      -v minimum="$minimum" '
    $0 == label { inside = 1; next }
    inside && /^_/ { exit count >= minimum ? 0 : 1 }
    inside && $0 ~ pattern { count++ }
    END { if (!inside || count < minimum) exit 1 }
  ' "$disasm"; then
    echo "ARM64 $symbol lacks $description" >&2
    exit 1
  fi
}

require_symbol_instruction_order() {
  symbol=$1
  first=$2
  second=$3
  description=$4
  if ! awk -v label="_$symbol:" -v first="$first" -v second="$second" '
    $0 == label { inside = 1; next }
    inside && /^_/ { exit ordered ? 0 : 1 }
    inside && $0 ~ first { seen_first = 1 }
    inside && seen_first && $0 ~ second { ordered = 1 }
    END { if (!inside || !ordered) exit 1 }
  ' "$disasm"; then
    echo "ARM64 $symbol lacks $description" >&2
    exit 1
  fi
}

require_shared_compare_reload_object() {
  if ! awk '
    function normhex(s, z) {
      sub(/^0x/, "", s)
      z = "0000000000000000" s
      return substr(z, length(z)-15)
    }
    $0 == "_lj_vmeta_comp:" { section = "comp"; next }
    $0 == "_lj_vmeta_equal:" { section = "equal"; next }
    /^_/ { section = "" }
    section == "comp" && $1 ~ /^[[:xdigit:]]+$/ {
      if (comp_state == 0 && $2 == "bl") {
        comp_state = 1
        next
      }
      if (comp_state == 1) {
        if ($2 != "ldr" || $3 != "x19," || $0 !~ /\[x23,/) bad = 1
        reload = normhex($1)
        comp_state = 2
        next
      }
      if (comp_state == 2) {
        if ($2 != "cmp" || $3 != "x0," || $4 != "#0x1") bad = 1
        comp_state = 3
      }
    }
    section == "equal" && $1 ~ /^[[:xdigit:]]+$/ {
      if (equal_state == 0 && $2 == "bl") {
        equal_state = 1
        next
      }
      if (equal_state == 1) {
        if ($2 != "b") bad = 1
        target = normhex($3)
        equal_state = 2
      }
    }
    END {
      exit !bad && comp_state == 3 && equal_state == 2 &&
           reload == target ? 0 : 1
    }
  ' "$disasm"; then
    echo "ARM64 equality/comparison object lacks exact shared BASE reload sequence" >&2
    exit 1
  fi
}

# The default build omits only the LJ_52 iterator-metatable probes checked
# above. Every unconditional mutable 64-bit consumer must lower to an address
# calculation followed by an acquire load in its generated VM symbol.
require_symbol_instruction_order lj_ff_type \
  '[[:space:]]add[[:space:]]+x14, x2, x9, lsl #3' \
  '[[:space:]]ldar[[:space:]]+x0, [[]x14[]]' \
  'C-closure type upvalue acquire sequence'
require_symbol_instruction_order lj_ff_tostring \
  '[[:space:]]add[[:space:]]+x14, x22, #0x[[:xdigit:]]+' \
  '[[:space:]]ldar[[:space:]]+x9, [[]x14[]]' \
  'numeric base-metatable root acquire sequence'
for symbol in lj_ff_pairs lj_ff_ipairs
do
  require_symbol_instruction_order "$symbol" \
    '[[:space:]]add[[:space:]]+x14, x2, #0x[[:xdigit:]]+' \
    '[[:space:]]ldar[[:space:]]+x3, [[]x14[]]' \
    'iterator C-closure upvalue acquire sequence'
done
require_symbol_instruction_order lj_ff_coroutine_wrap_aux \
  '[[:space:]]add[[:space:]]+x14, x2, #0x[[:xdigit:]]+' \
  '[[:space:]]ldar[[:space:]]+x0, [[]x14[]]' \
  'coroutine.wrap C-closure upvalue acquire sequence'
require_symbol_instruction_order lj_BC_UGET \
  '[[:space:]]add[[:space:]]+x14, x1, x28, lsl #3' \
  '[[:space:]]ldar[[:space:]]+x1, [[]x14[]]' \
  'UGET uvptr acquire sequence'
for symbol in lj_BC_USETV lj_BC_USETS lj_BC_USETN lj_BC_USETP
do
  require_symbol_instruction_order "$symbol" \
    '[[:space:]]add[[:space:]]+x14, x1, x27, lsl #3' \
    '[[:space:]]ldar[[:space:]]+x0, [[]x14[]]' \
    'USET uvptr acquire sequence'
done
require_symbol_instruction_order lj_BC_UCLO \
  '[[:space:]]add[[:space:]]+x14, x23, #0x[[:xdigit:]]+' \
  '[[:space:]]ldar[[:space:]]+x2, [[]x14[]]' \
  'open-upvalue head acquire sequence'
require_symbol_instruction_order lj_BC_FUNCCW \
  '[[:space:]]add[[:space:]]+x14, x22, #0x[[:xdigit:]]+' \
  '[[:space:]]ldar[[:space:]]+x3, [[]x14[]]' \
  'wrapper callback acquire sequence'

require_defined_text_symbol "$meta_nm_text" lj_meta_comp_rooted \
  'rooted comparison helper definition'
require_defined_text_symbol "$meta_nm_text" lj_meta_equal_rooted \
  'rooted equality helper definition'
for symbol in lj_BC_ISLT lj_BC_ISGE lj_BC_ISLE lj_BC_ISGT \
              lj_BC_ISEQV lj_BC_ISNEV
do
  require_defined_text_symbol "$nm_text" "$symbol" \
    'comparison bytecode definition'
done

require_symbol_relocation lj_vmeta_comp '_lj_meta_comp_rooted$' \
  '__lt/__le rooted helper relocation'
require_symbol_relocation lj_vmeta_equal '_lj_meta_equal_rooted$' \
  'rooted table __eq helper relocation'
require_symbol_relocation lj_vmeta_equal_cd '_lj_meta_equal_cd$' \
  'FFI __eq helper relocation'
require_symbol_relocation lj_vmeta_arith_vv '_lj_meta_arith$' \
  '__add helper relocation'
require_symbol_relocation lj_vmeta_len '_lj_meta_len$' \
  '__len helper relocation'
require_symbol_relocation lj_vmeta_call '_lj_meta_call$' \
  '__call helper relocation'
require_symbol_relocation lj_vmeta_callt '_lj_meta_call$' \
  'tail-call __call helper relocation'
require_symbol_relocation lj_BC_CAT '_lj_meta_cat$' \
  '__concat helper relocation'

for symbol in lj_vm_asm_begin lj_BC_ISGE lj_BC_ISLE lj_BC_ISGT
do
  require_symbol_instruction "$symbol" \
    '[[:space:]]b[.](lo|ne)[[:space:]]+_lj_vmeta_comp([[:space:]]|$)' \
    'ordered-comparison branch to vmeta_comp'
done

require_symbol_instruction lj_vmeta_comp \
  '[[:space:]]add[[:space:]]+x1, x19, x[0-9]+, lsl #3' \
  '__lt/__le lhs stack-root address'
require_symbol_instruction lj_vmeta_comp \
  '[[:space:]]add[[:space:]]+x2, x19, x[0-9]+, lsl #3' \
  '__lt/__le rhs stack-root address'
require_symbol_instruction_order lj_vmeta_comp \
  '[[:space:]]bl[[:space:]]' \
  '[[:space:]]ldr[[:space:]]+x19, [[]x23,' \
  '__lt/__le post-helper BASE reload'
for symbol in lj_BC_ISEQV lj_BC_ISNEV
do
  require_symbol_instruction_order "$symbol" \
    '[[:space:]]add[[:space:]]+x28, x19, x28, lsl #3' \
    '[[:space:]]mov[[:space:]]+x2, x28' \
    'table equality rhs stack rebase before helper argument'
  require_symbol_instruction "$symbol" \
    '[[:space:]]add[[:space:]]+x1, x19, x[0-9]+, lsl #3' \
    'table equality lhs stack-root address'
  require_symbol_instruction "$symbol" \
    '[[:space:]]mov[[:space:]]+x2, x28' \
    'table equality rhs stack-root address'
  require_symbol_instruction_order "$symbol" \
    '[[:space:]]mov[[:space:]]+x2, x28' \
    '[[:space:]]b[[:space:]]+_lj_vmeta_equal([[:space:]]|$)' \
    'table equality branch to rooted vmeta_equal'
done
require_shared_compare_reload_object

pub_reloc='_(lj_state_stack_dirty_vm|lj_state_stack_pubtv)$'
for symbol in \
  lj_cont_ra lj_cont_cat lj_BC_CAT lj_fff_res lj_vm_return lj_BC_RET
do
  require_symbol_relocation "$symbol" "$pub_reloc" \
    'metamethod result publication relocation'
done

require_symbol_relocation lj_ff_getmetatable \
  '_lj_meta_getmt_protected_rooted$' \
  'rooted protected getmetatable helper relocation'
require_symbol_instruction_order lj_ff_getmetatable \
  '[[:space:]](ldr|ldur)[[:space:]]+x21, [[]x19,' \
  '[[:space:]]str[[:space:]]+x21, [[]sp,' \
  'getmetatable frame-PC save ordering'
require_symbol_instruction_order lj_ff_getmetatable \
  '[[:space:]]mov[[:space:]]+x0, x23' \
  '[[:space:]]mov[[:space:]]+x1, x19' \
  'getmetatable L/object-root ABI'
require_symbol_instruction_order lj_ff_getmetatable \
  '[[:space:]]mov[[:space:]]+x1, x19' \
  '[[:space:]]sub[[:space:]]+x2, x19, #0x10' \
  'getmetatable object/output-root ABI'
require_symbol_instruction_order lj_ff_getmetatable \
  '[[:space:]]bl[[:space:]]' \
  '[[:space:]]ldr[[:space:]]+x19, [[]x23,' \
  'getmetatable post-helper BASE reload'
require_symbol_instruction_order lj_ff_getmetatable \
  '[[:space:]]ldr[[:space:]]+x19, [[]x23,' \
  '[[:space:]](ldr|ldur)[[:space:]]+x21, [[]x19,' \
  'getmetatable post-helper PC reload'
require_symbol_instruction_count lj_ff_getmetatable \
  '[[:space:]](ldr|ldur)[[:space:]]+x21, [[]x19,' 2 \
  'both pre-call and post-call frame-PC loads'
require_symbol_instruction lj_ff_getmetatable \
  '[[:space:]]b[[:space:]]+_lj_fff_res1([[:space:]]|$)' \
  'exact getmetatable fff_res1 branch'
require_symbol_instruction lj_ff_setmetatable \
  '[[:space:]]b[[:space:]]+_lj_fff_fallback([[:space:]]|$)' \
  'unconditional setmetatable C fallback branch'

require_object_symbol_relocation "$meta_nm_text" "$meta_relocs" \
  lj_meta_getmt_protected_rooted '_lj_tab_getstr_held_try$' \
  'protected getmetatable held-lookup relocation'
require_object_symbol_relocation "$meta_nm_text" "$meta_relocs" \
  lj_meta_getmt_protected_rooted '_lj_gc2_tv_lease_acquire$' \
  'protected getmetatable admission relocation'
require_object_symbol_relocation "$meta_nm_text" "$meta_relocs" \
  lj_meta_getmt_protected_rooted '_lj_state_stack_pubtv$' \
  'protected getmetatable stack-publication relocation'
require_object_symbol_relocation "$meta_nm_text" "$meta_relocs" \
  lj_meta_cat '_lj_state_stack_pubtv$' \
  '__concat C-frame publication relocation'
require_object_symbol_relocation "$meta_nm_text" "$meta_relocs" \
  lj_meta_call '_lj_state_stack_pubtv$' \
  '__call C-frame publication relocation'

if nm -u "$vm_object" "$meta_object" "$base_object" | \
   grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "ARM64 metamethod publication paths import an atomic runtime helper" >&2
  exit 1
fi

echo "arm64_meta_publication_contract OK: source and object families present"
