#!/bin/sh
# Run the M6 helper-backed JIT table-store guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TAB="$ROOT/src/lj_tab.c"
ASM="$ROOT/src/lj_asm_x86.h"
IRCALL="$ROOT/src/lj_ircall.h"

for helper in \
  'lj_tab_storetv_forjit_array_nogc' \
  'lj_tab_storetv_forjit_array' \
  'lj_tab_storetv_forjit_hash' \
  'lj_tab_storetv_forjit_newref'
do
  if ! grep -Fq "$helper, 5, S, PGC, CCI_L" "$IRCALL"; then
    printf 'required JIT table-store IRCALL missing: %s\n' "$helper" >&2
    exit 1
  fi
done

for route in \
  'IRCALL_lj_tab_storetv_forjit_array : IRCALL_lj_tab_storetv_forjit_hash' \
  'IRCALL_lj_tab_storetv_forjit_array_nogc' \
  'IRCALL_lj_tab_storetv_forjit_newref' \
  'expected helper-backed table store ref' \
  'asm_gencall(as, ci, args)'
do
  if ! grep -Fq "$route" "$ASM"; then
    printf 'required x64 table-store lowering route missing: %s\n' "$route" >&2
    exit 1
  fi
done

if ! awk '
  /^static void asm_ahstore_inline_array_num\(/ {
    in_fn = 1
    found = 1
    saw_helper = 0
    saw_lock = 0
    saw_fallback = 0
  }
  in_fn && /IRCALL_lj_tab_storetv_forjit_array_nogc/ { saw_helper = 1 }
  in_fn && /emit_i8\(as, 0xf0\)/ { saw_lock = 1 }
  in_fn && /emit_sjcc\(as, CC_NE, l_fallback\)/ { saw_fallback = 1 }
  in_fn && /^}/ {
    if (!(saw_helper && saw_lock && saw_fallback))
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$ASM"; then
  printf '%s\n' 'numeric ASTORE inline path must keep lock-CAS gate and helper fallback' >&2
  exit 1
fi

if ! awk '
  /^static void asm_ahstore_inline_hash_num\(/ {
    in_fn = 1
    found = 1
    saw_helper = 0
    saw_lock = 0
    saw_fallback = 0
  }
  in_fn && /IRCALL_lj_tab_storetv_forjit_hash/ { saw_helper = 1 }
  in_fn && /emit_i8\(as, 0xf0\)/ { saw_lock = 1 }
  in_fn && /emit_sjcc\(as, CC_NE, l_fallback\)/ { saw_fallback = 1 }
  in_fn && /^}/ {
    if (!(saw_helper && saw_lock && saw_fallback))
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$ASM"; then
  printf '%s\n' 'numeric HSTORE inline path must keep lock-CAS gate and helper fallback' >&2
  exit 1
fi

check_keyed_store_fn() {
  fn=$1
  if ! awk -v fn="$fn" '
    function track_braces(line) {
      opens = gsub(/\{/, "{", line)
      line = $0
      closes = gsub(/\}/, "}", line)
      if (opens)
	body = 1
      depth += opens - closes
      if (body && depth == 0)
	in_fn = 0
    }
    !in_fn &&
    $0 ~ "^[[:space:]]*LJ_FUNCA .*" fn "[[:space:]]*\\(" {
      in_fn = 1; body = 0; depth = 0; saw_keyed = 0; saw_wait = 0
    }
    in_fn && /lj_tab_trystoretv_cas_keyed[[:space:]]*\(/ { saw_keyed = 1 }
    in_fn && /lj_tab_store_wait_no_l[[:space:]]*\(/ { saw_wait = 1 }
    in_fn { track_braces($0) }
    END { exit(saw_keyed && saw_wait ? 0 : 1) }
  ' "$TAB"; then
    printf '%s\n' "$fn must CAS through lj_tab_trystoretv_cas_keyed() and wait via lj_tab_store_wait_no_l()" >&2
    exit 1
  fi
}

check_keyed_store_fn lj_tab_storetv_forjit_array_nogc
check_keyed_store_fn lj_tab_storetv_forvm_array
check_keyed_store_fn lj_tab_storetv_forjit_hash
check_keyed_store_fn lj_tab_storetv_forjit_newref
check_keyed_store_fn lj_tab_storetvn_forvm_array

if ! awk '
  /^LJ_FUNCA TValue \*lj_tab_storetv_forjit_array\(/ { in_fn = 1; found = 1 }
  in_fn && /lj_tab_storetv_forjit_array_nogc/ { saw_nogc = 1 }
  in_fn && /lj_gc2_barrier_weak_write/ { saw_weak = 1 }
  in_fn && /lj_gc2_barrier_tv_pair/ { saw_pair = 1 }
  in_fn && /^}/ {
    if (!(saw_nogc && saw_weak && saw_pair))
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'GC-capable ASTORE helper must route through no-GC CAS helper and run weak/parent barriers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m6_jit_table_store_helper
